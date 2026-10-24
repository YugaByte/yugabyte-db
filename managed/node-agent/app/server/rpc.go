// Copyright (c) YugabyteDB, Inc.

package server

import (
	"bufio"
	"context"
	"crypto/tls"
	"errors"
	"io"
	"io/fs"
	"net"
	"net/http"
	"node-agent/app/task"
	"node-agent/app/ybdb"
	pb "node-agent/generated/service"
	"node-agent/metric"
	"node-agent/model"
	"node-agent/util"
	"os"
	"path/filepath"
	"time"

	"node-agent/cmux"

	"golang.org/x/net/http2"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/encoding/gzip"
	"google.golang.org/grpc/keepalive"
	"google.golang.org/grpc/status"
)

var (
	// Enforcement policy for keepalive.
	// Clients should set keepalive time to a value greater than this minimum.
	minKeepaliveTime           = 30 * time.Second
	loadedCertExpirySecs int64 = 1
)

// RPCServer is the struct for gRPC server.
type RPCServer struct {
	listener     net.Listener
	gServer      *grpc.Server
	metricServer *http.Server
	isTLS        bool
	done         chan struct{}
}

// RPCServerConfig is the config for RPC server.
type RPCServerConfig struct {
	// Address is the server address.
	Address string
	// EnableTLS is to enable TLS for both RPC and metric servers.
	EnableTLS bool
	// EnableMetrics is to enable metrics.
	EnableMetrics bool
	// DisableMetricsTLS is to disable TLS for metrics when EnableTLS is to true.
	DisableMetricsTLS bool
}

// NewRPCServer creates an instance of gRPC server.
func NewRPCServer(
	ctx context.Context,
	serverConfig *RPCServerConfig,
) (*RPCServer, error) {
	serverOpts := []grpc.ServerOption{}
	unaryInterceptors := []grpc.UnaryServerInterceptor{UnaryPanicHandler()}
	streamInterceptors := []grpc.StreamServerInterceptor{StreamPanicHandler()}
	if serverConfig.EnableMetrics {
		unaryInterceptors = append(unaryInterceptors, UnaryMetricHandler())
		streamInterceptors = append(streamInterceptors, StreamMetricHandler())
	}
	var tlsConfig *tls.Config
	if serverConfig.EnableTLS {
		config, err := loadTLSConfig()
		if err != nil {
			util.FileLogger().Errorf(ctx, "Error in loading TLS credentials: %s", err)
			return nil, err
		}
		// Find the expiry seconds for the server certificate on startup.
		certExpirySecs, err := util.CertExpirySecs(&config.Certificates[0])
		if err != nil {
			util.FileLogger().Errorf(ctx, "Error in getting server cert expiry: %s", err)
			return nil, err
		}
		// Store it globally for the RPC server to use.
		loadedCertExpirySecs = certExpirySecs
		tlsConfig = config
	}
	listener, err := net.Listen("tcp", serverConfig.Address)
	if err != nil {
		util.FileLogger().Errorf(ctx, "Failed to listen to %s: %v", serverConfig.Address, err)
		return nil, err
	}
	if serverConfig.EnableTLS {
		if serverConfig.DisableMetricsTLS {
			// Let gRPC handle TLS for RPC server.
			serverOpts = append(serverOpts, grpc.Creds(credentials.NewTLS(tlsConfig)))
		} else {
			// Create a new listener with TLS for both RPC and metrics server.
			listener = tls.NewListener(
				listener,
				util.TlsConfig(tlsConfig.Certificates, []string{"http/1.1", http2.NextProtoTLS}),
			)
		}
	}
	if serverConfig.EnableTLS {
		authenticator := &Authenticator{util.CurrentConfig()}
		unaryInterceptors = append(unaryInterceptors, authenticator.UnaryInterceptor())
		streamInterceptors = append(streamInterceptors, authenticator.StreamInterceptor())
	}
	mux := cmux.New(listener)
	mListener := mux.Match(cmux.HTTP1())
	gListener := mux.Match(cmux.Any())
	serverOpts = append(serverOpts, grpc.KeepaliveEnforcementPolicy(keepalive.EnforcementPolicy{
		MinTime:             minKeepaliveTime,
		PermitWithoutStream: false,
	}))
	serverOpts = append(serverOpts, grpc.ChainUnaryInterceptor(unaryInterceptors...))
	serverOpts = append(serverOpts, grpc.ChainStreamInterceptor(streamInterceptors...))
	gServer := grpc.NewServer(serverOpts...)
	server := &RPCServer{
		listener:     listener,
		gServer:      gServer,
		metricServer: &http.Server{},
		isTLS:        serverConfig.EnableTLS,
		done:         make(chan struct{}),
	}
	pb.RegisterNodeAgentServer(gServer, server)
	metric.GetInstance().PrepopulateMetrics(gServer)
	http.Handle("/metrics", metric.GetInstance().HTTPHandler())
	// Start metrics server.
	go func() {
		if err := server.metricServer.Serve(mListener); err != nil {
			util.FileLogger().Errorf(ctx, "Exiting metrics service: %v", err)
		}
		select {
		case <-server.done:
			return
		default:
			close(server.done)
		}

	}()
	// Start RPC server.
	go func() {
		if err := gServer.Serve(gListener); err != nil {
			util.FileLogger().Errorf(ctx, "Exiting RPC service: %v", err)
		}
		select {
		case <-server.done:
			return
		default:
			close(server.done)
		}
	}()
	// Start the root listener.
	go func() {
		if err := mux.Serve(); err != nil {
			util.FileLogger().Errorf(ctx, "Exiting server: %v", err)
		}
		select {
		case <-server.done:
			return
		default:
			close(server.done)
		}
	}()
	return server, nil
}

func loadTLSConfig() (*tls.Config, error) {
	config := util.CurrentConfig()
	certFilePath := util.ServerCertPath(config)
	keyFilepath := util.ServerKeyPath(config)
	serverCert, err := tls.LoadX509KeyPair(certFilePath, keyFilepath)
	if err != nil {
		return nil, err
	}
	return util.TlsConfig([]tls.Certificate{serverCert}, nil), nil
}

func removeFileIfPresent(filename string) error {
	stat, err := os.Stat(filename)
	if err == nil {
		if stat.IsDir() {
			err = errors.New("Path already exists as a directory")
		} else {
			err = os.Remove(filename)
		}
	} else if errors.Is(err, fs.ErrNotExist) {
		err = nil
	}
	return err
}

// Addr returns the server address.
func (server *RPCServer) Addr() string {
	var addr string
	if server.listener != nil {
		addr = server.listener.Addr().String()
	}
	return addr
}

// Done returns a channel to check the status of the services.
func (server *RPCServer) Done() <-chan struct{} {
	return server.done
}

// Stop stops the services.
func (server *RPCServer) Stop() {
	if server.gServer != nil {
		server.gServer.GracefulStop()
	}
	server.gServer = nil
	if server.metricServer != nil {
		server.metricServer.Close()
	}
	server.metricServer = nil
	if server.listener != nil {
		server.listener.Close()
	}
	server.listener = nil
}

func toGrpcErrorIfNeeded(code codes.Code, err error) error {
	if err == nil {
		return err
	}
	_, ok := status.FromError(err)
	if ok {
		return err
	}
	return status.Error(code, err.Error())
}

/* Implementation of gRPC methods start here. */

// Ping handles ping request.
func (server *RPCServer) Ping(ctx context.Context, in *pb.PingRequest) (*pb.PingResponse, error) {
	util.FileLogger().Debugf(ctx, "Received ping")
	config := util.CurrentConfig()
	_, err := os.Stat(task.RootNodeExporterUnit)
	return &pb.PingResponse{
		ServerInfo: &pb.ServerInfo{
			Version:              config.String(util.PlatformVersionKey),
			RestartNeeded:        config.Bool(util.NodeAgentRestartKey),
			Compressor:           gzip.Name,
			CertExpirySecs:       loadedCertExpirySecs,
			RootNodeExporterUnit: err == nil,
		},
	}, nil
}

// ExecuteCommand executes a command on the server.
func (server *RPCServer) ExecuteCommand(
	req *pb.ExecuteCommandRequest,
	stream pb.NodeAgent_ExecuteCommandServer,
) error {
	var res *pb.ExecuteCommandResponse
	ctx := stream.Context()
	cmd := req.GetCommand()
	username := req.GetUser()
	shellTask := task.NewShellTaskWithUser("RemoteCommand", username, cmd[0], cmd[1:])
	out, err := shellTask.Process(ctx)
	if err == nil {
		res = &pb.ExecuteCommandResponse{
			Data: &pb.ExecuteCommandResponse_Output{
				Output: out.Info.String(),
			},
		}
	} else {
		util.FileLogger().Errorf(ctx, "Error in running command: %s - %s", cmd, err.Error())
		res = &pb.ExecuteCommandResponse{
			Data: &pb.ExecuteCommandResponse_Error{
				Error: &pb.Error{
					Code:    int32(out.ExitStatus.Code),
					Message: out.ExitStatus.Error.String(),
				},
			},
		}
	}
	err = stream.Send(res)
	if err != nil {
		util.FileLogger().Errorf(ctx, "Error in sending response - %s", err.Error())
		return status.Error(codes.Internal, err.Error())
	}
	return nil
}

// SubmitTask submits an async task on the server.
func (server *RPCServer) SubmitTask(
	ctx context.Context,
	req *pb.SubmitTaskRequest,
) (*pb.SubmitTaskResponse, error) {
	taskID := req.GetTaskId()
	username := req.GetUser()
	cmdInput := req.GetCommandInput()
	res := &pb.SubmitTaskResponse{TaskId: taskID}
	if cmdInput != nil {
		// Handle generic shell commands.
		cmd := cmdInput.GetCommand()
		shellTask := task.NewShellTaskWithUser("RemoteCommand", username, cmd[0], cmd[1:])
		err := task.GetTaskManager().Submit(ctx, taskID, shellTask)
		if err != nil {
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		return res, nil
	}
	preflightCheckInput := req.GetPreflightCheckInput()
	if preflightCheckInput != nil {
		// Handle preflight check RPC.
		preflightCheckParam := &model.PreflightCheckParam{}
		err := util.ConvertType(preflightCheckInput, &preflightCheckParam)
		if err != nil {
			util.FileLogger().Errorf(ctx, "Error in preflight input conversion - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.InvalidArgument, err)
		}
		preflightCheckHandler := task.NewPreflightCheckHandler(preflightCheckParam)
		err = task.GetTaskManager().
			Submit(ctx, taskID, preflightCheckHandler)
		if err != nil {
			util.FileLogger().Errorf(ctx, "Error in running preflight check - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		return res, nil
	}
	downloadSoftwareInput := req.GetDownloadSoftwareInput()
	if downloadSoftwareInput != nil {
		downloadSoftwareHandler := task.NewDownloadSoftwareHandler(downloadSoftwareInput, username)
		err := task.GetTaskManager().Submit(ctx, taskID, downloadSoftwareHandler)
		if err != nil {
			util.FileLogger().Errorf(ctx, "Error in running download software - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		res.TaskId = taskID
		return res, nil
	}
	installSoftwareInput := req.GetInstallSoftwareInput()
	if installSoftwareInput != nil {
		installSoftwareHandler := task.NewInstallSoftwareHandler(installSoftwareInput, username)
		err := task.GetTaskManager().Submit(ctx, taskID, installSoftwareHandler)
		if err != nil {
			util.FileLogger().Errorf(ctx, "Error in running install software - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		res.TaskId = taskID
		return res, nil
	}
	serverControlInput := req.GetServerControlInput()
	if serverControlInput != nil {
		// Handle server control RPC.
		serverControlHandler := task.NewServerControlHandler(serverControlInput, username)
		err := task.GetTaskManager().Submit(ctx, taskID, serverControlHandler)
		if err != nil {
			util.FileLogger().Errorf(ctx, "Error in running server control - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		res.TaskId = taskID
		return res, nil
	}
	configureServiceInput := req.GetConfigureServiceInput()
	if configureServiceInput != nil {
		switch configureServiceInput.GetService() {
		case pb.Service_EARLYOOM:
			configureHandler := task.NewConfigureServiceHandler(
				configureServiceInput.GetConfig(),
				configureServiceInput.GetEnabled(),
			)
			err2 := task.GetTaskManager().Submit(ctx, taskID, configureHandler)
			if err2 != nil {
				util.FileLogger().
					Errorf(ctx, "Error in running configure handler - %s", err2.Error())
				return res, toGrpcErrorIfNeeded(codes.Internal, err2)
			}
			res.TaskId = taskID
			return res, nil
		default:
			return res, status.Errorf(
				codes.Unimplemented,
				"Unsupported type: %s", configureServiceInput.GetService(),
			)
		}
	}
	serverGFlagsInput := req.GetServerGFlagsInput()
	if serverGFlagsInput != nil {
		// Handle server gflags RPC.
		serverGFlagsHandler := task.NewServerGflagsHandler(serverGFlagsInput, username)
		err := task.GetTaskManager().Submit(ctx, taskID, serverGFlagsHandler)
		if err != nil {
			util.FileLogger().Errorf(ctx, "Error in running server gflags - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		res.TaskId = taskID
		return res, nil
	}
	installYbcInput := req.GetInstallYbcInput()
	if installYbcInput != nil {
		installYbcHandler := task.NewInstallYbcHandler(installYbcInput, username)
		err := task.GetTaskManager().Submit(ctx, taskID, installYbcHandler)
		if err != nil {
			util.FileLogger().Errorf(ctx, "Error in running install ybc - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		res.TaskId = taskID
		return res, nil
	}
	configureServerInput := req.GetConfigureServerInput()
	if configureServerInput != nil {
		configureServerHandler := task.NewConfigureServerHandler(configureServerInput, username)
		err := task.GetTaskManager().Submit(ctx, taskID, configureServerHandler)
		if err != nil {
			util.FileLogger().Errorf(ctx, "Error in running configure server - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		res.TaskId = taskID
		return res, nil
	}
	installOtelCollectorInput := req.GetInstallOtelCollectorInput()
	if installOtelCollectorInput != nil {
		installOtelCollectorHandler := task.NewInstallOtelCollectorHandler(
			installOtelCollectorInput,
			username,
		)
		err := task.GetTaskManager().Submit(ctx, taskID, installOtelCollectorHandler)
		if err != nil {
			util.FileLogger().
				Errorf(ctx, "Error in running install otel collector - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		res.TaskId = taskID
		return res, nil
	}
	configureCloudFederationInput := req.GetConfigureCloudFederationInput()
	if configureCloudFederationInput != nil {
		configureCloudFederationHandler := task.NewConfigureCloudFederationHandler(
			configureCloudFederationInput,
			username,
		)
		err := task.GetTaskManager().Submit(ctx, taskID, configureCloudFederationHandler)
		if err != nil {
			util.FileLogger().
				Errorf(ctx, "Error in running configure cloud federation - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		res.TaskId = taskID
		return res, nil
	}
	configureNodeExporterInput := req.GetConfigureNodeExporterInput()
	if configureNodeExporterInput != nil {
		configureNodeExporterHandler := task.NewConfigureNodeExporterHandler(
			configureNodeExporterInput,
			username,
		)
		err := task.GetTaskManager().Submit(ctx, taskID, configureNodeExporterHandler)
		if err != nil {
			util.FileLogger().
				Errorf(ctx, "Error in running configure node exporter - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		res.TaskId = taskID
		return res, nil
	}
	setupCGroupInput := req.GetSetupCGroupInput()
	if setupCGroupInput != nil {
		setupCgroupHandler := task.NewSetupCgroupHandler(
			setupCGroupInput,
			username,
		)
		err := task.GetTaskManager().Submit(ctx, taskID, setupCgroupHandler)
		if err != nil {
			util.FileLogger().
				Errorf(ctx, "Error in running setup cGroup - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		res.TaskId = taskID
		return res, nil
	}
	destroyServerInput := req.GetDestroyServerInput()
	if destroyServerInput != nil {
		destroyServerHandler := task.NewDestroyServerHandler(
			destroyServerInput,
			username,
		)
		err := task.GetTaskManager().Submit(ctx, taskID, destroyServerHandler)
		if err != nil {
			util.FileLogger().
				Errorf(ctx, "Error in running destroy server - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		res.TaskId = taskID
		return res, nil
	}
	ynpPreflightCheckInput := req.GetYnpPreflightCheckInput()
	if ynpPreflightCheckInput != nil {
		ynpPreflightCheckHandler := task.NewYnpPreflightCheckHandler(ynpPreflightCheckInput)
		err := task.GetTaskManager().Submit(ctx, taskID, ynpPreflightCheckHandler)
		if err != nil {
			util.FileLogger().Errorf(ctx, "Error in running YNP preflight check - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		res.TaskId = taskID
		return res, nil
	}
	rotateSshKeyInput := req.GetRotateSshKeyInput()
	if rotateSshKeyInput != nil {
		rotateSshKeyHandler := task.NewRotateSSHKeyHandler(rotateSshKeyInput, username)
		err := task.GetTaskManager().Submit(ctx, taskID, rotateSshKeyHandler)
		if err != nil {
			util.FileLogger().Errorf(ctx, "Error in running rotate SSH key - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		res.TaskId = taskID
		return res, nil
	}
	healthCheckInput := req.GetHealthCheckInput()
	if healthCheckInput != nil {
		healthCheckHandler := task.NewHealthCheckerHandler(healthCheckInput, username)
		err := task.GetTaskManager().Submit(ctx, taskID, healthCheckHandler)
		if err != nil {
			util.FileLogger().Errorf(ctx, "Error in running health check - %s", err.Error())
			return res, toGrpcErrorIfNeeded(codes.Internal, err)
		}
		res.TaskId = taskID
		return res, nil
	}
	return res, toGrpcErrorIfNeeded(codes.Unimplemented, errors.New("Unknown task"))
}

// DescribeTask describes a submitted task.
// Client needs to retry on DeadlineExeeced error.
func (server *RPCServer) DescribeTask(
	req *pb.DescribeTaskRequest,
	stream pb.NodeAgent_DescribeTaskServer,
) error {
	ctx := stream.Context()
	taskID := req.GetTaskId()
	err := task.GetTaskManager().Subscribe(
		ctx,
		taskID,
		func(callbackData *task.TaskCallbackData) error {
			var res *pb.DescribeTaskResponse
			if callbackData.ExitCode == 0 {
				if callbackData.RPCResponse != nil {
					res = callbackData.RPCResponse
				} else {
					res = &pb.DescribeTaskResponse{
						State: callbackData.State.String(),
						Data: &pb.DescribeTaskResponse_Output{
							Output: callbackData.Info,
						},
					}
				}
			} else {
				res = &pb.DescribeTaskResponse{
					State: callbackData.State.String(),
					Data: &pb.DescribeTaskResponse_Error{
						Error: &pb.Error{
							Code:    int32(callbackData.ExitCode),
							Message: callbackData.Error,
						},
					},
				}
			}
			err := stream.Send(res)
			if err != nil {
				util.FileLogger().Errorf(ctx, "Error in sending response - %s", err.Error())
				return err
			}
			return nil
		})
	if err != nil && err != io.EOF {
		return toGrpcErrorIfNeeded(codes.Internal, err)
	}
	return nil
}

// AbortTask aborts a running task.
func (server *RPCServer) AbortTask(
	ctx context.Context,
	req *pb.AbortTaskRequest,
) (*pb.AbortTaskResponse, error) {
	taskID, err := task.GetTaskManager().Abort(ctx, req.GetTaskId())
	if err != nil {
		return nil, toGrpcErrorIfNeeded(codes.Internal, err)
	}
	return &pb.AbortTaskResponse{TaskId: taskID}, nil
}

// UploadFile handles upload file to a specified file.
func (server *RPCServer) UploadFile(stream pb.NodeAgent_UploadFileServer) error {
	ctx := stream.Context()
	req, err := stream.Recv()
	if err != nil {
		util.FileLogger().Errorf(ctx, "Error in receiving file info - %s", err.Error())
		return toGrpcErrorIfNeeded(codes.Internal, err)
	}
	fileInfo := req.GetFileInfo()
	filename := fileInfo.GetFilename()
	username := req.GetUser()
	chmod := req.GetChmod()
	userDetail, err := util.UserInfo(username)
	if err != nil {
		return toGrpcErrorIfNeeded(codes.Internal, err)
	}
	util.FileLogger().Debugf(ctx, "Using user: %s, uid: %d, gid: %d",
		userDetail.User.Username, userDetail.UserID, userDetail.GroupID)
	if !filepath.IsAbs(filename) {
		filename = filepath.Join(userDetail.User.HomeDir, filename)
	}
	if chmod == 0 {
		// Set the default file mode.
		chmod = 0666
	}
	// Remove the file if it already exists.
	err = removeFileIfPresent(filename)
	if err != nil {
		util.FileLogger().Errorf(
			ctx, "Error in deleting existing file %s - %s", filename, err.Error())
		return toGrpcErrorIfNeeded(codes.Internal, err)
	}
	util.FileLogger().Infof(ctx, "Setting file permission for %s to %o", filename, chmod)
	file, err := os.OpenFile(filename, os.O_TRUNC|os.O_RDWR|os.O_CREATE, fs.FileMode(chmod))
	if err != nil {
		util.FileLogger().Errorf(ctx, "Error in creating file %s - %s", filename, err.Error())
		return toGrpcErrorIfNeeded(codes.Internal, err)
	}
	defer file.Close()
	if !userDetail.IsCurrent {
		err = file.Chown(int(userDetail.UserID), int(userDetail.GroupID))
		if err != nil {
			util.FileLogger().
				Errorf(ctx, "Error in changing file owner %s - %s", filename, err.Error())
			return toGrpcErrorIfNeeded(codes.Internal, err)
		}
	}
	// Flushes 4K bytes by default.
	writer := bufio.NewWriter(file)
	for {
		req, err = stream.Recv()
		if err == io.EOF {
			err = writer.Flush()
			if err == nil {
				break
			}
			util.FileLogger().
				Errorf(ctx, "Error in flushing data to file %s - %s", filename, err.Error())
			return toGrpcErrorIfNeeded(codes.Internal, err)
		}
		if err != nil {
			util.FileLogger().Errorf(ctx, "Error in reading from stream - %s", err.Error())
			return toGrpcErrorIfNeeded(codes.Internal, err)
		}
		chunk := req.GetChunkData()
		size := len(chunk)
		util.FileLogger().Debugf(ctx, "Received a chunk with size: %d", size)
		_, err = writer.Write(chunk)
		if err != nil {
			util.FileLogger().Errorf(ctx, "Error in writing to file %s - %s", filename, err.Error())
			return toGrpcErrorIfNeeded(codes.Internal, err)
		}
	}
	res := &pb.UploadFileResponse{}
	err = stream.SendAndClose(res)
	if err != nil {
		util.FileLogger().Errorf(ctx, "Error in sending response - %s", err.Error())
		return toGrpcErrorIfNeeded(codes.Internal, err)
	}
	return nil
}

// DownloadFile downloads a specified file.
func (server *RPCServer) DownloadFile(
	in *pb.DownloadFileRequest,
	stream pb.NodeAgent_DownloadFileServer,
) error {
	ctx := stream.Context()
	filename := in.GetFilename()
	res := &pb.DownloadFileResponse{ChunkData: make([]byte, 1024)}
	if !filepath.IsAbs(filename) {
		username := in.GetUser()
		userDetail, err := util.UserInfo(username)
		if err != nil {
			return toGrpcErrorIfNeeded(codes.Internal, err)
		}
		util.FileLogger().Debugf(ctx, "Using user: %s, uid: %d, gid: %d",
			userDetail.User.Username, userDetail.UserID, userDetail.GroupID)
		filename = filepath.Join(userDetail.User.HomeDir, filename)
	}
	file, err := os.Open(filename)
	if err != nil {
		util.FileLogger().Errorf(ctx, "Error in opening file %s - %s", filename, err.Error())
		return toGrpcErrorIfNeeded(codes.Internal, err)
	}
	defer file.Close()
	// Reads 4K bytes by default.
	reader := bufio.NewReader(file)
	for {
		n, err := reader.Read(res.ChunkData)
		if err != nil && err != io.EOF {
			util.FileLogger().Errorf(ctx, "Error in reading file %s - %s", filename, err.Error())
			return toGrpcErrorIfNeeded(codes.Internal, err)
		}
		if n > 0 {
			res.ChunkData = res.ChunkData[:n]
			sendErr := stream.Send(res)
			if sendErr != nil {
				util.FileLogger().
					Errorf(ctx, "Error in sending file %s - %s", filename, sendErr.Error())
				return toGrpcErrorIfNeeded(codes.Internal, sendErr)
			}
		}
		if err == io.EOF {
			break
		}
	}
	return nil
}

// Update updates the config file on upgrade.
func (server *RPCServer) Update(
	ctx context.Context,
	in *pb.UpdateRequest,
) (*pb.UpdateResponse, error) {
	var err error
	config := util.CurrentConfig()
	state := in.GetState()
	switch state {
	case model.Upgrade.Name():
		// Start the upgrade process as all the files are available.
		err = HandleUpgradeState(ctx, config, in.GetUpgradeInfo())
	case model.Upgraded.Name():
		// Platform has confirmed that it has also rotated the cert and the key.
		err = HandleUpgradedState(ctx, config)
	default:
		err = status.Errorf(codes.InvalidArgument, "Unhandled state - %s", state)
	}
	res := &pb.UpdateResponse{Home: util.MustGetHomeDirectory()}
	return res, err
}

// CheckYugabyteDbStatus detects the running YugabyteDB related processes on the
// node, the ports they listen on and the configuration they were started with.
func (server *RPCServer) CheckYugabyteDbStatus(
	ctx context.Context,
	in *pb.CheckYugabyteDbStatusRequest,
) (*pb.CheckYugabyteDbStatusResponse, error) {
	util.FileLogger().Debugf(ctx, "Received check YugabyteDB status")
	res, err := ybdb.NewDetector().Detect(ctx)
	if err != nil {
		util.FileLogger().Errorf(ctx, "Error in detecting YugabyteDB status - %s", err.Error())
		return nil, toGrpcErrorIfNeeded(codes.Internal, err)
	}
	return res, nil
}

/* End of gRPC methods. */
