// Copyright (c) YugabyteDB, Inc.

package com.yugabyte.yw.common;

import com.fasterxml.jackson.core.type.TypeReference;
import com.google.api.client.util.Throwables;
import com.google.common.annotations.VisibleForTesting;
import com.google.common.base.Stopwatch;
import com.google.common.cache.CacheBuilder;
import com.google.common.cache.CacheLoader;
import com.google.common.cache.LoadingCache;
import com.google.common.collect.ImmutableList;
import com.google.common.util.concurrent.ThreadFactoryBuilder;
import com.google.inject.Inject;
import com.google.protobuf.ByteString;
import com.google.protobuf.Descriptors.FieldDescriptor;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.commissioner.NodeAgentPoller;
import com.yugabyte.yw.common.certmgmt.CertificateHelper;
import com.yugabyte.yw.common.config.GlobalConfKeys;
import com.yugabyte.yw.common.config.RuntimeConfGetter;
import com.yugabyte.yw.common.config.UniverseConfKeys;
import com.yugabyte.yw.common.logging.LogUtil;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.models.NodeAgent;
import com.yugabyte.yw.models.NodeAgent.State;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeExporterSystemdLevel;
import com.yugabyte.yw.models.helpers.YBAError;
import com.yugabyte.yw.nodeagent.AbortTaskRequest;
import com.yugabyte.yw.nodeagent.CheckYugabyteDbStatusRequest;
import com.yugabyte.yw.nodeagent.CheckYugabyteDbStatusResponse;
import com.yugabyte.yw.nodeagent.ConfigureCloudFederationInput;
import com.yugabyte.yw.nodeagent.ConfigureCloudFederationOutput;
import com.yugabyte.yw.nodeagent.ConfigureNodeExporterInput;
import com.yugabyte.yw.nodeagent.ConfigureNodeExporterOutput;
import com.yugabyte.yw.nodeagent.ConfigureServerInput;
import com.yugabyte.yw.nodeagent.ConfigureServerOutput;
import com.yugabyte.yw.nodeagent.ConfigureServiceInput;
import com.yugabyte.yw.nodeagent.ConfigureServiceOutput;
import com.yugabyte.yw.nodeagent.DescribeTaskRequest;
import com.yugabyte.yw.nodeagent.DescribeTaskResponse;
import com.yugabyte.yw.nodeagent.DestroyServerInput;
import com.yugabyte.yw.nodeagent.DestroyServerOutput;
import com.yugabyte.yw.nodeagent.DownloadFileRequest;
import com.yugabyte.yw.nodeagent.DownloadFileResponse;
import com.yugabyte.yw.nodeagent.DownloadSoftwareInput;
import com.yugabyte.yw.nodeagent.DownloadSoftwareOutput;
import com.yugabyte.yw.nodeagent.ExecuteCommandRequest;
import com.yugabyte.yw.nodeagent.ExecuteCommandResponse;
import com.yugabyte.yw.nodeagent.FileInfo;
import com.yugabyte.yw.nodeagent.HealthCheckInput;
import com.yugabyte.yw.nodeagent.HealthCheckOutput;
import com.yugabyte.yw.nodeagent.InstallOtelCollectorInput;
import com.yugabyte.yw.nodeagent.InstallOtelCollectorOutput;
import com.yugabyte.yw.nodeagent.InstallSoftwareInput;
import com.yugabyte.yw.nodeagent.InstallSoftwareOutput;
import com.yugabyte.yw.nodeagent.InstallYbcInput;
import com.yugabyte.yw.nodeagent.InstallYbcOutput;
import com.yugabyte.yw.nodeagent.NodeAgentGrpc;
import com.yugabyte.yw.nodeagent.NodeAgentGrpc.NodeAgentBlockingStub;
import com.yugabyte.yw.nodeagent.NodeAgentGrpc.NodeAgentStub;
import com.yugabyte.yw.nodeagent.PingRequest;
import com.yugabyte.yw.nodeagent.PingResponse;
import com.yugabyte.yw.nodeagent.PreflightCheckInput;
import com.yugabyte.yw.nodeagent.PreflightCheckOutput;
import com.yugabyte.yw.nodeagent.RotateSshKeyInput;
import com.yugabyte.yw.nodeagent.RotateSshKeyOutput;
import com.yugabyte.yw.nodeagent.ServerControlInput;
import com.yugabyte.yw.nodeagent.ServerControlOutput;
import com.yugabyte.yw.nodeagent.ServerGFlagsInput;
import com.yugabyte.yw.nodeagent.ServerGFlagsOutput;
import com.yugabyte.yw.nodeagent.SetupCGroupInput;
import com.yugabyte.yw.nodeagent.SetupCGroupOutput;
import com.yugabyte.yw.nodeagent.SubmitTaskRequest;
import com.yugabyte.yw.nodeagent.SubmitTaskResponse;
import com.yugabyte.yw.nodeagent.UpdateRequest;
import com.yugabyte.yw.nodeagent.UpdateResponse;
import com.yugabyte.yw.nodeagent.UpgradeInfo;
import com.yugabyte.yw.nodeagent.UploadFileRequest;
import com.yugabyte.yw.nodeagent.UploadFileResponse;
import com.yugabyte.yw.nodeagent.YnpPreflightCheckInput;
import com.yugabyte.yw.nodeagent.YnpPreflightCheckOutput;
import io.grpc.CallOptions;
import io.grpc.Channel;
import io.grpc.ClientCall;
import io.grpc.ClientInterceptor;
import io.grpc.ConnectivityState;
import io.grpc.Context;
import io.grpc.Deadline;
import io.grpc.ForwardingClientCall;
import io.grpc.ManagedChannel;
import io.grpc.Metadata;
import io.grpc.MethodDescriptor;
import io.grpc.Status.Code;
import io.grpc.StatusRuntimeException;
import io.grpc.netty.shaded.io.grpc.netty.GrpcSslContexts;
import io.grpc.netty.shaded.io.grpc.netty.NettyChannelBuilder;
import io.grpc.netty.shaded.io.netty.channel.ChannelOption;
import io.grpc.netty.shaded.io.netty.handler.ssl.SslContext;
import io.grpc.stub.ClientCallStreamObserver;
import io.grpc.stub.ClientResponseObserver;
import io.grpc.stub.StreamObserver;
import io.jsonwebtoken.Jwts;
import io.jsonwebtoken.SignatureAlgorithm;
import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.PrivateKey;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.StringTokenizer;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.stream.Collectors;
import javax.annotation.Nullable;
import javax.inject.Singleton;
import javax.net.ssl.SSLException;
import lombok.Builder;
import lombok.Getter;
import lombok.extern.slf4j.Slf4j;
import org.apache.commons.collections4.MapUtils;
import org.apache.commons.collections4.queue.CircularFifoQueue;
import org.apache.commons.lang3.StringUtils;
import org.apache.commons.lang3.builder.HashCodeBuilder;
import org.slf4j.MDC;
import org.slf4j.Marker;
import org.slf4j.MarkerFactory;
import play.libs.Json;

/** This class contains all the methods required to communicate to a node agent server. */
@Slf4j
@Singleton
public class NodeAgentClient {
  public static final String NODE_AGENT_SERVICE_CONFIG_FILE = "node_agent/service_config.json";
  public static final int FILE_UPLOAD_CHUNK_SIZE_BYTES = 4096;
  public static final int MAX_TRANSIENT_FAILURES = 2;
  // Cap captured node-agent output included in task failures shown in the UI.
  private static final int MAX_ERROR_OUTPUT_CHARS = 4000;
  // Node-agent can stream a single chunk up to ~1MB; cap each stored line.
  private static final int MAX_ERROR_OUTPUT_LINE_CHARS = 1024;

  // Cache of the channels for re-use.
  private final LoadingCache<ChannelConfig, ManagedChannel> cachedChannels;

  private final ChannelFactory channelFactory;
  private final RuntimeConfGetter confGetter;
  // Late binding to prevent circular dependency.
  private final com.google.inject.Provider<NodeAgentPoller> nodeAgentPollerProvider;

  @Inject
  public NodeAgentClient(
      RuntimeConfGetter confGetter,
      PlatformExecutorFactory platformExecutorFactory,
      com.google.inject.Provider<NodeAgentPoller> nodeAgentPollerProvider) {
    this(
        confGetter,
        nodeAgentPollerProvider,
        platformExecutorFactory.createExecutor(
            "node_agent.grpc_executor",
            new ThreadFactoryBuilder().setNameFormat("NodeAgentGrpcPool-%d").build()));
  }

  public NodeAgentClient(
      RuntimeConfGetter confGetter,
      com.google.inject.Provider<NodeAgentPoller> nodeAgentPollerProvider,
      ExecutorService executorService) {
    this(
        confGetter,
        nodeAgentPollerProvider,
        config ->
            ChannelFactory.getDefaultChannel(
                config, new GrpcClientRequestInterceptor(config, confGetter), executorService));
  }

  public NodeAgentClient(
      RuntimeConfGetter confGetter,
      com.google.inject.Provider<NodeAgentPoller> nodeAgentPollerProvider,
      ChannelFactory channelFactory) {
    this.confGetter = confGetter;
    this.nodeAgentPollerProvider = nodeAgentPollerProvider;
    this.channelFactory = channelFactory;
    CacheBuilder<Object, Object> cacheBuilder =
        CacheBuilder.newBuilder()
            .removalListener(
                n -> {
                  ManagedChannel channel = (ManagedChannel) n.getValue();
                  log.debug(
                      "Channel for {} expired. Current size: {}", n.getKey(), getClientCacheSize());
                  if (!channel.isShutdown() && !channel.isTerminated()) {
                    channel.shutdown();
                  }
                })
            .expireAfterAccess(10, TimeUnit.MINUTES);
    if (confGetter.getGlobalConf(GlobalConfKeys.nodeAgentIgnoreConnectionCacheSize)) {
      // Only LRU is effective.
      log.debug("Ignoring max cache size for node agent client connections");
    } else {
      int maxClients = confGetter.getGlobalConf(GlobalConfKeys.nodeAgentConnectionCacheSize);
      log.debug("Setting max cache size for node agent client connections to {}", maxClients);
      cacheBuilder = cacheBuilder.maximumSize(maxClients);
    }
    this.cachedChannels =
        cacheBuilder.build(
            new CacheLoader<ChannelConfig, ManagedChannel>() {
              @Override
              public ManagedChannel load(ChannelConfig config) {
                return NodeAgentClient.this.channelFactory.get(config);
              }
            });
  }

  @Builder
  public static class NodeAgentUpgradeParam {
    @Nullable private String certDir;
    @Nullable private Path packagePath;
  }

  /** This class intercepts the client request to add the authorization token header. */
  public static class GrpcClientRequestInterceptor implements ClientInterceptor {
    private final ChannelConfig config;
    private final RuntimeConfGetter confGetter;

    GrpcClientRequestInterceptor(ChannelConfig config, RuntimeConfGetter confGetter) {
      this.config = config;
      this.confGetter = confGetter;
    }

    public <ReqT, RespT> ClientCall<ReqT, RespT> interceptCall(
        MethodDescriptor<ReqT, RespT> methodDescriptor, CallOptions callOptions, Channel channel) {
      Duration tokenLifetime = confGetter.getGlobalConf(GlobalConfKeys.nodeAgentTokenLifetime);
      String compression = config.getNodeAgent().getConfig().getCompressor();
      if (callOptions.getDeadline() == null) {
        // Set the default deadline if is not set.
        callOptions =
            callOptions.withDeadlineAfter(tokenLifetime.toMillis(), TimeUnit.MILLISECONDS);
      }
      if (StringUtils.isNotBlank(compression)
          && confGetter.getGlobalConf(GlobalConfKeys.nodeAgentEnableMessageCompression)) {
        callOptions = callOptions.withCompression(compression);
      }
      int requestLogLevel = confGetter.getGlobalConf(GlobalConfKeys.nodeAgentServerRequestLogLevel);
      return new ForwardingClientCall.SimpleForwardingClientCall<ReqT, RespT>(
          channel.newCall(methodDescriptor, callOptions)) {

        @Override
        public void start(ClientCall.Listener<RespT> responseListener, Metadata headers) {
          UUID nodeAgentUuid = config.getNodeAgent().getUuid();
          log.trace("Setting custom headers for node agent {}", nodeAgentUuid);
          String correlationId = MDC.get(LogUtil.CORRELATION_ID);
          if (StringUtils.isEmpty(correlationId)) {
            correlationId = UUID.randomUUID().toString();
          }
          String requestId = UUID.randomUUID().toString();
          log.debug(
              "Using correlation ID {} and request ID {} for node agent {}",
              correlationId,
              requestId,
              nodeAgentUuid);
          String token = NodeAgentClient.getNodeAgentJWT(nodeAgentUuid, tokenLifetime);
          headers.put(Metadata.Key.of("authorization", Metadata.ASCII_STRING_MARSHALLER), token);
          headers.put(
              Metadata.Key.of("x-correlation-id", Metadata.ASCII_STRING_MARSHALLER), correlationId);
          headers.put(Metadata.Key.of("x-request-id", Metadata.ASCII_STRING_MARSHALLER), requestId);
          if (requestLogLevel >= 0) {
            headers.put(
                Metadata.Key.of("x-request-log-level", Metadata.ASCII_STRING_MARSHALLER),
                String.valueOf(requestLogLevel));
          }
          super.start(responseListener, headers);
        }
      };
    }
  }

  @FunctionalInterface
  public interface ChannelFactory {
    ManagedChannel get(ChannelConfig config);

    static ManagedChannel getDefaultChannel(
        ChannelConfig config, ClientInterceptor interceptor, ExecutorService executor) {
      NodeAgent nodeAgent = config.nodeAgent;
      NettyChannelBuilder channelBuilder =
          NettyChannelBuilder.forAddress(nodeAgent.getIp(), nodeAgent.getPort())
              .idleTimeout(config.getIdleTimeout().toMinutes(), TimeUnit.MINUTES)
              .keepAliveTime(config.getKeepAliveTime().toSeconds(), TimeUnit.SECONDS)
              .keepAliveTimeout(config.keepAliveTimeout.toSeconds(), TimeUnit.SECONDS)
              .withOption(
                  ChannelOption.CONNECT_TIMEOUT_MILLIS, (int) config.getConnectTimeout().toMillis())
              .keepAliveWithoutCalls(false)
              // Override the default cached pool.
              .executor(executor)
              .offloadExecutor(executor);
      if (config.isEnableTls()) {
        try {
          String certPath = config.getCertPath().toString();
          log.trace("Using cert path {} for node agent {}", certPath, config.nodeAgent);
          SslContext sslcontext =
              GrpcSslContexts.forClient()
                  .trustManager(CertificateHelper.getCertsFromFile(certPath))
                  .build();
          channelBuilder = channelBuilder.sslContext(sslcontext);
          channelBuilder.intercept(interceptor);
        } catch (SSLException e) {
          String errMsg =
              String.format(
                  "SSL context creation for gRPC client failed for node agent %s", nodeAgent);
          log.error(errMsg);
          throw new RuntimeException(errMsg, e);
        }
      } else {
        channelBuilder = channelBuilder.usePlaintext();
      }
      Map<String, ?> serviceConfig = getServiceConfig(NODE_AGENT_SERVICE_CONFIG_FILE);
      if (MapUtils.isNotEmpty(serviceConfig)) {
        channelBuilder
            .disableServiceConfigLookUp()
            .defaultServiceConfig(serviceConfig)
            .enableRetry();
      }
      return channelBuilder.build();
    }

    public static Map<String, ?> getServiceConfig(String configFile) {
      try (InputStream inputStream =
          ChannelFactory.class.getClassLoader().getResourceAsStream(configFile)) {
        Map<String, ?> map =
            Json.mapper()
                .readValue(
                    Objects.requireNonNull(
                        inputStream, "Node agent service config file is not found"),
                    new TypeReference<Map<String, ?>>() {});
        return fixMap(map);
      } catch (IOException e) {
        throw new RuntimeException(e);
      }
    }

    // A hack to convert Number to Double because ManagedChannelImplBuilder allows only Double
    // instance type for numbers.
    static Map<String, ?> fixMap(Map<String, ?> map) {
      return Util.transformMap(map, x -> (x instanceof Number) ? ((Number) x).doubleValue() : x);
    }
  }

  @Getter
  @Builder
  public static class ChannelConfig {
    private NodeAgent nodeAgent;
    private boolean enableTls;
    private Path certPath;
    private Duration connectTimeout;
    private Duration idleTimeout;
    private Duration keepAliveTime;
    private Duration keepAliveTimeout;

    @Override
    public int hashCode() {
      return new HashCodeBuilder(17, 37)
          .append(nodeAgent.getUuid())
          .append(enableTls)
          .append(certPath)
          .append(connectTimeout)
          .append(idleTimeout)
          .append(keepAliveTime)
          .append(keepAliveTimeout)
          .toHashCode();
    }

    @Override
    public boolean equals(Object obj) {
      if (this == obj) {
        return true;
      }
      if (obj == null) {
        return false;
      }
      if (getClass() != obj.getClass()) {
        return false;
      }
      ChannelConfig other = (ChannelConfig) obj;
      if (!Objects.equals(nodeAgent.getUuid(), other.nodeAgent.getUuid())) {
        return false;
      }
      if (!Objects.equals(nodeAgent.getIp(), other.nodeAgent.getIp())) {
        return false;
      }
      if (!Objects.equals(nodeAgent.getVersion(), other.nodeAgent.getVersion())) {
        return false;
      }
      if (enableTls != other.enableTls) {
        return false;
      }
      if (!Objects.equals(certPath, other.certPath)) {
        return false;
      }
      if (!Objects.equals(connectTimeout, other.connectTimeout)) {
        return false;
      }
      if (!Objects.equals(idleTimeout, other.idleTimeout)) {
        return false;
      }
      if (!Objects.equals(keepAliveTime, other.keepAliveTime)) {
        return false;
      }
      if (!Objects.equals(keepAliveTimeout, other.keepAliveTimeout)) {
        return false;
      }
      return true;
    }

    @Override
    public String toString() {
      return nodeAgent.toString();
    }
  }

  @Slf4j
  static class BaseResponseObserver<T> implements StreamObserver<T> {
    private final String id;
    private final String correlationId;
    private final CountDownLatch latch = new CountDownLatch(1);
    private Throwable throwable;

    BaseResponseObserver(String id) {
      this.id = id;
      this.correlationId = MDC.get(LogUtil.CORRELATION_ID);
    }

    protected void setCorrelationId() {
      if (!StringUtils.isBlank(correlationId)) {
        MDC.put(LogUtil.CORRELATION_ID, correlationId);
      }
    }

    @Override
    public void onNext(T response) {
      setCorrelationId();
    }

    @Override
    public void onError(Throwable throwable) {
      setCorrelationId();
      this.throwable = throwable;
      latch.countDown();
    }

    @Override
    public void onCompleted() {
      setCorrelationId();
      latch.countDown();
      log.info("Completed for {}", getId());
    }

    protected String getId() {
      return id;
    }

    public Throwable getThrowable() {
      return throwable;
    }

    public void waitFor() {
      waitFor(true);
    }

    public void waitFor(boolean throwOnError) {
      try {
        latch.await();
      } catch (InterruptedException e) {
        throw new RuntimeException(e);
      }
      if (throwOnError && throwable != null) {
        Throwables.propagate(throwable);
      }
    }
  }

  static class StreamingClientResponseObserver<Req, Res> extends BaseResponseObserver<Res>
      implements ClientResponseObserver<Req, Res> {
    private static final int REQUEST_COUNT = 1;
    private final Supplier<Req> requestSupplier;
    private volatile ClientCallStreamObserver<Req> requestStream;

    StreamingClientResponseObserver(String id, Supplier<Req> requestSupplier) {
      super(id);
      this.requestSupplier = requestSupplier;
    }

    // Initialization of the stream observer.
    @Override
    public void beforeStart(ClientCallStreamObserver<Req> requestStream) {
      this.requestStream = requestStream;
      // Set up manual flow control for the response stream.
      this.requestStream.disableAutoRequestWithInitial(REQUEST_COUNT);
      this.requestStream.setOnReadyHandler(
          () -> {
            setCorrelationId();
            while (requestStream.isReady()) {
              Req data = requestSupplier.get();
              if (data == null) {
                requestStream.onCompleted();
              } else {
                requestStream.onNext(data);
              }
            }
          });
    }

    @Override
    public void onNext(Res response) {
      super.onNext(response);
      // Signal to produce count more messages to be delivered to the 'inbound' StreamObserver.
      requestStream.request(REQUEST_COUNT);
    }
  }

  static class ExecuteCommandResponseObserver extends BaseResponseObserver<ExecuteCommandResponse> {
    private final boolean logOutput;
    private final StringBuilder stdOut;
    private final StringBuilder stdErr;
    private final AtomicInteger exitCode;

    ExecuteCommandResponseObserver(String id, boolean logOutput) {
      super(id);
      this.logOutput = logOutput;
      this.stdOut = new StringBuilder();
      this.stdErr = new StringBuilder();
      this.exitCode = new AtomicInteger();
    }

    @Override
    public void onNext(ExecuteCommandResponse response) {
      super.onNext(response);
      Marker fileMarker = MarkerFactory.getMarker("fileOnly");
      if (response.hasError()) {
        exitCode.set(response.getError().getCode());
        String errMsg = response.getError().getMessage();
        if (logOutput) {
          log.debug(fileMarker, errMsg);
        }
        stdErr.append(errMsg);
        onError(
            new RuntimeException(
                formatNodeAgentFailure(
                    String.format(
                        "Error(%d) %s",
                        response.getError().getCode(), response.getError().getMessage()),
                    stdOut.toString())));
      } else {
        String msg = response.getOutput();
        if (logOutput) {
          log.debug(fileMarker, msg);
        }
        stdOut.append(msg);
      }
    }

    public ShellResponse getResponse() {
      waitFor(false);
      ShellResponse response = new ShellResponse();
      if (exitCode.get() != 0) {
        response.code = exitCode.get();
        response.message = getStdErr();
      } else if (getThrowable() != null) {
        response.code = ShellResponse.ERROR_CODE_GENERIC_ERROR;
        response.message = getThrowable().getMessage();
      } else {
        response.message = getStdOut();
      }
      return response;
    }

    public String getStdOut() {
      return stdOut.toString();
    }

    public String getStdErr() {
      return stdErr.toString();
    }

    public int getExitCode() {
      return exitCode.get();
    }
  }

  static class DownloadFileResponseObserver extends BaseResponseObserver<DownloadFileResponse> {
    private final OutputStream outputStream;

    DownloadFileResponseObserver(String id, OutputStream outputStream) {
      super(id);
      this.outputStream = outputStream;
    }

    @Override
    public void onNext(DownloadFileResponse response) {
      try {
        super.onNext(response);
        outputStream.write(response.getChunkData().toByteArray());
      } catch (IOException e) {
        onError(e);
      }
    }
  }

  static class DescribeTaskResponseObserver<T> extends BaseResponseObserver<DescribeTaskResponse> {
    private final Class<T> responseClass;
    private final AtomicReference<T> resultRef;
    private final CircularFifoQueue<String> outputBuffer;

    DescribeTaskResponseObserver(
        String id, Class<T> responseClass, CircularFifoQueue<String> outputBuffer) {
      super(id);
      this.responseClass = responseClass;
      this.resultRef = new AtomicReference<>();
      this.outputBuffer = outputBuffer;
    }

    public T waitForResponse() {
      super.waitFor();
      return resultRef.get();
    }

    @Override
    public void onNext(DescribeTaskResponse response) {
      try {
        super.onNext(response);
        if (response.hasError()) {
          onError(new RuntimeException(formatDescribeTaskError(response)));
        } else {
          if (response.hasOutput()) {
            String chunk = response.getOutput();
            addOutputLines(outputBuffer, chunk);
            log.info(chunk);
          } else {
            for (Map.Entry<FieldDescriptor, Object> entry : response.getAllFields().entrySet()) {
              if (entry.getValue() == null) {
                continue;
              }
              if (entry.getValue().getClass().isAssignableFrom(responseClass)) {
                resultRef.set(responseClass.cast(entry.getValue()));
                break;
              }
            }
          }
        }
      } catch (Exception e) {
        onError(e);
      }
    }

    private String formatDescribeTaskError(DescribeTaskResponse response) {
      com.yugabyte.yw.nodeagent.Error error = response.getError();
      StringBuilder sb = new StringBuilder();
      sb.append("Code: ").append(error.getCode());
      sb.append(", Error: ").append(error.getMessage());
      if (StringUtils.isNotBlank(response.getState())) {
        sb.append(", State: ").append(response.getState());
      }
      return formatNodeAgentFailure(sb.toString(), String.join("\n", outputBuffer));
    }
  }

  private static void addOutputLines(CircularFifoQueue<String> outputBuffer, String chunk) {
    if (StringUtils.isEmpty(chunk)) {
      return;
    }
    StringTokenizer tokenizer = new StringTokenizer(chunk, "\n");
    while (tokenizer.hasMoreTokens()) {
      String line = tokenizer.nextToken();
      if (StringUtils.isBlank(line)) {
        continue;
      }
      outputBuffer.add(StringUtils.abbreviateMiddle(line, "...", MAX_ERROR_OUTPUT_LINE_CHARS));
    }
  }

  @VisibleForTesting
  static String formatNodeAgentFailure(String summary, String output) {
    String captured = StringUtils.trimToEmpty(output);
    if (StringUtils.isBlank(captured)) {
      return summary;
    }
    StringBuilder sb = new StringBuilder();
    sb.append(summary);
    sb.append(", Output:\n");
    sb.append(StringUtils.abbreviateMiddle(captured, "...", MAX_ERROR_OUTPUT_CHARS));
    return sb.toString();
  }

  public static String getNodeAgentJWT(NodeAgent nodeAgent, Duration tokenLifetime) {
    PrivateKey privateKey = nodeAgent.getSignerPrivateKey();
    return Jwts.builder()
        .setIssuer("https://www.yugabyte.com")
        .setSubject("Platform")
        .setExpiration(Date.from(Instant.now().plusSeconds(tokenLifetime.getSeconds())))
        .signWith(privateKey, SignatureAlgorithm.RS512)
        .compact();
  }

  public static String getNodeAgentJWT(UUID nodeAgentUuid, Duration tokenLifetime) {
    Optional<NodeAgent> nodeAgentOp = NodeAgent.maybeGet(nodeAgentUuid);
    if (!nodeAgentOp.isPresent()) {
      throw new RuntimeException(String.format("Node agent %s does not exist", nodeAgentUuid));
    }
    return getNodeAgentJWT(nodeAgentOp.get(), tokenLifetime);
  }

  public void addNodeAgentClientParams(
      NodeAgent nodeAgent, List<String> cmdParams, Map<String, String> redactedVals) {
    Duration tokenLifetime = confGetter.getGlobalConf(GlobalConfKeys.nodeAgentTokenLifetime);
    String token = getNodeAgentJWT(nodeAgent, tokenLifetime);
    cmdParams.add("--node_agent_ip");
    cmdParams.add(nodeAgent.getIp());
    cmdParams.add("--node_agent_port");
    cmdParams.add(String.valueOf(nodeAgent.getPort()));
    cmdParams.add("--node_agent_cert_path");
    cmdParams.add(nodeAgent.getCaCertFilePath().toString());
    cmdParams.add("--node_agent_home");
    cmdParams.add(nodeAgent.getHome());
    cmdParams.add("--node_agent_auth_token");
    cmdParams.add(token);
    redactedVals.put(token, "REDACTED");
  }

  /**
   * Gets the node agent for the given IP and triggers upgrade if needed. It returns empty optional
   * if node agent is not found or not in active state.
   *
   * @param ip the IP address of the node agent.
   * @return the optional of node agent instance.
   */
  public Optional<NodeAgent> maybeGetAndUpgrade(String ip) {
    Optional<NodeAgent> optional = NodeAgent.maybeGetByIp(ip);
    if (optional.isEmpty()) {
      log.debug("Node agent is not installed for {}", ip);
      return optional;
    }
    NodeAgent nodeAgent = optional.get();
    if (!nodeAgent.isActive()) {
      log.debug("Node agent {} is not in active state", nodeAgent);
      return Optional.empty();
    }
    if (nodeAgentPollerProvider.get().upgradeNodeAgent(nodeAgent.getUuid())) {
      nodeAgent.refresh();
    }
    return optional;
  }

  /**
   * Gets the node agent for the given IP and triggers upgrade if needed. It throws exception if
   * node agent is not found or not in active state.
   *
   * @param ip the IP address of the node agent.
   * @return the node agent instance.
   */
  public NodeAgent getAndUpgradeOrThrow(String ip) {
    return maybeGetAndUpgrade(ip)
        .orElseThrow(
            () ->
                new IllegalStateException(
                    String.format(
                        "Node agent must already be installed and in active state for %s", ip)));
  }

  /**
   * Returns true if the cloudType supports node agent.
   *
   * @param cloudType the given cloud type.
   * @return true if the cloudType supports node agent.
   */
  public static boolean isCloudTypeSupported(CloudType cloudType) {
    if (cloudType == CloudType.kubernetes || cloudType == CloudType.local) {
      log.trace("Node agent is not supported for kubernetes provider");
      return false;
    }
    return true;
  }

  /**
   * Checks if node agent client is enabled for the provider and the universe if it is non-null.
   * Client check adds additional requirements.
   *
   * @param provider the given provider.
   * @param universe the given universe.
   * @return true if the client is enabled.
   */
  public boolean isClientEnabled(Provider provider, @Nullable Universe universe) {
    if (!isCloudTypeSupported(provider.getCloudCode())) {
      log.debug(
          "Node agent is not supported for {} provider {}",
          provider.getCloudCode(),
          provider.getUuid());
      return false;
    }
    if (universe != null && !isNodeAgentEnabled(universe, p -> true).orElse(false)) {
      log.debug(
          "Node agent is not enabled for universe {}({})",
          universe.getName(),
          universe.getUniverseUUID());
      return false;
    }
    if (universe != null && universe.getUniverseDetails().installNodeAgent) {
      log.debug(
          "Node agent is not available on all nodes for universe {}({})",
          universe.getName(),
          universe.getUniverseUUID());
      // Check if mixed mode is allowed.
      if (!confGetter.getConfForScope(universe, UniverseConfKeys.allowNodeAgentClientMixMode)) {
        return false;
      }
    }
    // All checks passed.
    return true;
  }

  // This checks if node agent is enabled for the universe with additional provider check.
  public Optional<Boolean> isNodeAgentEnabled(
      Universe universe, Predicate<Provider> additionalProviderPredicate) {

    Map<UUID, Boolean> providerEnabledMap = new HashMap<>();
    for (Cluster cluster : universe.getUniverseDetails().clusters) {

      boolean isK8s =
          cluster.userIntent != null
              && cluster.userIntent.getAllCloudTypes().contains(CloudType.kubernetes);

      if (cluster.userIntent == null || cluster.userIntent.getAllCloudTypes().isEmpty() || isK8s) {
        // Unsupported cluster is found.
        return Optional.empty();
      }
      boolean enabled = true;
      for (UUID providerUUID : cluster.userIntent.getAllProviderUUIDs()) {
        enabled =
            enabled
                && providerEnabledMap.computeIfAbsent(
                    providerUUID,
                    k -> {
                      Provider provider = Provider.getOrBadRequest(providerUUID);
                      boolean isEnabled =
                          additionalProviderPredicate == null
                              || additionalProviderPredicate.test(provider);
                      if (!isEnabled) {
                        log.debug(
                            "Node agent is not enabled for provider {} in additional check",
                            provider.getUuid());
                      }
                      return isEnabled;
                    });
      }
      if (!enabled) {
        return Optional.of(false);
      }
    }
    return Optional.of(universe.getUniverseDetails().clusters.size() > 0);
  }

  private ManagedChannel getManagedChannel(NodeAgent nodeAgent, boolean enableTls) {
    Duration connectTimeout = confGetter.getGlobalConf(GlobalConfKeys.nodeAgentConnectTimeout);
    Duration idleTimeout = confGetter.getGlobalConf(GlobalConfKeys.nodeAgentIdleConnectionTimeout);
    Duration keepAliveTime =
        confGetter.getGlobalConf(GlobalConfKeys.nodeAgentConnectionKeepAliveTime);
    Duration keepAliveTimeout =
        confGetter.getGlobalConf(GlobalConfKeys.nodeAgentConnectionKeepAliveTimeout);
    ChannelConfig.ChannelConfigBuilder builder =
        ChannelConfig.builder()
            .nodeAgent(nodeAgent)
            .enableTls(enableTls)
            .connectTimeout(connectTimeout)
            .idleTimeout(idleTimeout)
            .keepAliveTime(keepAliveTime)
            .keepAliveTimeout(keepAliveTimeout);
    if (enableTls) {
      Path certPath = nodeAgent.getCaCertFilePath();
      if (nodeAgent.getState() == State.UPGRADE || nodeAgent.getState() == State.UPGRADED) {
        // Upgrade state is also considered just in case the DB update fails (very rare).
        // Node agent may still be running with old cert.
        Path mergedCertPath = nodeAgent.getMergedCaCertFilePath();
        if (mergedCertPath.toFile().exists()) {
          certPath = mergedCertPath;
        }
      }
      builder.certPath(certPath);
    }
    try {
      ChannelConfig channelConfig = builder.build();
      ManagedChannel channel = cachedChannels.get(channelConfig);
      if (isChannelUnhealthy(channel)) {
        cachedChannels.invalidate(channelConfig);
        cachedChannels.cleanUp();
        channel = cachedChannels.get(channelConfig);
      }
      return channel;
    } catch (ExecutionException e) {
      throw new RuntimeException(e.getCause());
    }
  }

  private boolean isChannelUnhealthy(ManagedChannel channel) {
    if (channel.isShutdown() || channel.isTerminated()) {
      return true;
    }
    ConnectivityState state = channel.getState(true);
    return state == ConnectivityState.SHUTDOWN || state == ConnectivityState.TRANSIENT_FAILURE;
  }

  public PingResponse ping(NodeAgent nodeAgent) {
    return ping(nodeAgent, true);
  }

  public PingResponse ping(NodeAgent nodeAgent, boolean enableTls) {
    ManagedChannel channel = getManagedChannel(nodeAgent, enableTls);
    NodeAgentBlockingStub stub = NodeAgentGrpc.newBlockingStub(channel);
    return stub.ping(PingRequest.newBuilder().build());
  }

  /**
   * Detects the running YugabyteDB related processes on the node, the ports they listen on and the
   * configuration they were started with. This is a synchronous, read-only call.
   *
   * @param nodeAgent the node agent to query.
   * @return the detected status of each known role.
   */
  public CheckYugabyteDbStatusResponse checkYugabyteDbStatus(NodeAgent nodeAgent) {
    ManagedChannel channel = getManagedChannel(nodeAgent, true);
    NodeAgentBlockingStub stub = NodeAgentGrpc.newBlockingStub(channel);
    return stub.checkYugabyteDbStatus(CheckYugabyteDbStatusRequest.newBuilder().build());
  }

  public PingResponse waitForServerReady(NodeAgent nodeAgent, Duration timeout) {
    Stopwatch stopwatch = Stopwatch.createStarted();
    while (true) {
      try {
        PingResponse response = ping(nodeAgent);
        nodeAgent.updateServerInfo(response.getServerInfo());
        return response;
      } catch (StatusRuntimeException e) {
        nodeAgent.updateLastError(new YBAError(YBAError.Code.CONNECTION_ERROR, e.getMessage()));
        if (e.getStatus().getCode() != Code.UNAVAILABLE
            && e.getStatus().getCode() != Code.DEADLINE_EXCEEDED) {
          log.error("Error in connecting to Node agent {} - {}", nodeAgent, e.getStatus());
          throw e;
        }
        log.warn("Node agent {} is not reachable", nodeAgent);
        if (stopwatch.elapsed().compareTo(timeout) > 0) {
          throw e;
        }
        try {
          Thread.sleep(1000);
        } catch (InterruptedException ex) {
          throw new RuntimeException(ex);
        }
        log.info("Retrying connection validation to node agent {}", nodeAgent);
      } catch (RuntimeException e) {
        log.error("Error in connecting to node agent {} - {}", nodeAgent, e.getMessage());
        throw e;
      }
    }
  }

  public ShellResponse executeCommand(
      NodeAgent nodeAgent, List<String> command, ShellProcessContext context) {
    return executeCommand(nodeAgent, command, context, false);
  }

  /**
   * node_exporter layout on the node, from its unit files: USER if the DB user has a user-level
   * unit, ROOT if only the root-level one exists, empty if neither does.
   */
  public Optional<NodeExporterSystemdLevel> detectNodeExporterSystemdLevel(NodeAgent nodeAgent) {
    // systemd reads user units from the user's passwd home, which can differ from the provider's
    // yb home; bash resolves ~user from passwd.
    String userUnit =
        "~" + NodeManager.YUGABYTE_USER + "/.config/systemd/user/node_exporter.service";
    if (executeCommand(nodeAgent, ImmutableList.of("bash", "-c", "test -f " + userUnit)).code
        == 0) {
      return Optional.of(NodeExporterSystemdLevel.USER);
    }
    String rootUnit = "/etc/systemd/system/node_exporter.service";
    if (executeCommand(nodeAgent, ImmutableList.of("test", "-f", rootUnit)).code == 0) {
      return Optional.of(NodeExporterSystemdLevel.ROOT);
    }
    return Optional.empty();
  }

  public ShellResponse executeCommand(NodeAgent nodeAgent, List<String> command) {
    // Use the user of the node-agent process by not setting a specific user.
    return executeCommand(
        nodeAgent,
        command,
        ShellProcessContext.DEFAULT.toBuilder().useDefaultUser(false).build(),
        false);
  }

  public ShellResponse executeCommand(
      NodeAgent nodeAgent, List<String> command, ShellProcessContext context, boolean useBash) {
    ManagedChannel channel = getManagedChannel(nodeAgent, true);
    NodeAgentStub stub = NodeAgentGrpc.newStub(channel);
    String id = String.format("%s-%s", nodeAgent.getUuid(), command.get(0));
    ExecuteCommandResponseObserver responseObserver =
        new ExecuteCommandResponseObserver(id, context.isLogCmdOutput());
    ExecuteCommandRequest.Builder builder =
        ExecuteCommandRequest.newBuilder()
            .addAllCommand(useBash ? getBashCommand(command) : command);
    String user = context.getSshUser();
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    if (context.getTimeoutSecs() > 0L) {
      stub = stub.withDeadlineAfter(context.getTimeoutSecs(), TimeUnit.SECONDS);
    }
    List<String> redactedCommand = context.redactCommand(command);
    String description =
        context.getDescription() == null
            ? StringUtils.abbreviateMiddle(String.join(" ", redactedCommand), " ... ", 140)
            : context.getDescription();
    String logMsg = String.format("Starting proc (abbrev cmd) - %s", description);
    if (context.isTraceLogging()) {
      log.trace(logMsg);
    } else {
      log.info(logMsg);
    }
    try {
      if (context.isLogCmdOutput()) {
        log.debug("Proc stdout for '{}' :", description);
      }
      stub.executeCommand(builder.build(), responseObserver);
      ShellResponse response = responseObserver.getResponse();
      response.setDescription(description);
      return response;
    } catch (Throwable e) {
      log.error("Error in running command. Error: {}", e.getMessage());
      throw new RuntimeException("Command execution failed. Error: " + e.getMessage(), e);
    }
  }

  public ShellResponse executeScript(
      NodeAgent nodeAgent, Path scriptPath, List<String> params, ShellProcessContext context) {
    try {
      byte[] bytes = Files.readAllBytes(scriptPath);
      // Use a unique heredoc terminator so it can't collide with content inside the script body
      // (e.g. a 'cat <<EOF ... EOF' block in the user's script).
      String heredocMarker = "YBA_SCRIPT_EOF_" + UUID.randomUUID().toString().replace("-", "");
      String quotedParams =
          params.stream().map(NodeAgentClient::shellQuote).collect(Collectors.joining(" "));
      ImmutableList.Builder<String> commandBuilder = ImmutableList.builder();
      commandBuilder.add("/bin/bash").add("-c");
      commandBuilder.add(
          String.format(
              "/bin/bash -s -- %s <<'%s'\n%s\n%s",
              quotedParams,
              heredocMarker,
              new String(bytes, StandardCharsets.UTF_8),
              heredocMarker));
      List<String> command = commandBuilder.build();
      return executeCommand(nodeAgent, command, context);
    } catch (Exception e) {
      log.error("Error in running script {}", scriptPath, e);
      if (e instanceof RuntimeException) {
        throw (RuntimeException) e;
      }
      throw new RuntimeException(e);
    }
  }

  /**
   * Wraps a string in single quotes for safe interpolation into a bash command line. Embedded
   * single quotes are escaped using the standard '"'"' idiom (close-quote, escaped-quote,
   * re-open-quote).
   */
  private static String shellQuote(String s) {
    return "'" + s.replace("'", "'\\''") + "'";
  }

  public void uploadFile(NodeAgent nodeAgent, String inputFile, String outputFile) {
    uploadFile(nodeAgent, inputFile, outputFile, null, 0, null);
  }

  public void uploadFile(
      NodeAgent nodeAgent,
      String inputFile,
      String outputFile,
      String user,
      int chmod,
      Duration timeout) {
    ManagedChannel channel = getManagedChannel(nodeAgent, true);
    try (InputStream inputStream = new BufferedInputStream(new FileInputStream(inputFile))) {
      NodeAgentStub stub = NodeAgentGrpc.newStub(channel);
      String id = String.format("%s-%s", nodeAgent.getUuid(), inputFile);
      FileInfo fileInfo = FileInfo.newBuilder().setFilename(outputFile).build();
      if (timeout != null && !timeout.isZero()) {
        stub = stub.withDeadlineAfter(timeout.toMillis(), TimeUnit.MILLISECONDS);
      }
      AtomicBoolean isMetadataSent = new AtomicBoolean();
      byte[] bytes = new byte[FILE_UPLOAD_CHUNK_SIZE_BYTES];
      StreamingClientResponseObserver<UploadFileRequest, UploadFileResponse> responseObserver =
          new StreamingClientResponseObserver<>(
              id,
              () -> {
                if (isMetadataSent.compareAndSet(false, true)) {
                  UploadFileRequest.Builder builder =
                      UploadFileRequest.newBuilder().setFileInfo(fileInfo);
                  if (StringUtils.isNotBlank(user)) {
                    builder.setUser(user);
                  }
                  if (chmod > 0) {
                    builder.setChmod(chmod);
                  }
                  return builder.build();
                }
                try {
                  int bytesRead = inputStream.read(bytes);
                  return bytesRead > 0
                      ? UploadFileRequest.newBuilder()
                          .setChunkData(ByteString.copyFrom(bytes, 0, bytesRead))
                          .build()
                      : null;
                } catch (IOException e) {
                  throw new RuntimeException(e);
                }
              });
      stub.uploadFile(responseObserver);
      responseObserver.waitFor();
    } catch (Exception e) {
      throw new RuntimeException(
          String.format(
              "Error in uploading file %s to %s. Error: %s", inputFile, outputFile, e.getMessage()),
          e);
    }
  }

  public void downloadFile(NodeAgent nodeAgent, String inputFile, String outputFile) {
    downloadFile(nodeAgent, inputFile, outputFile, null, null);
  }

  public void downloadFile(
      NodeAgent nodeAgent, String inputFile, String outputFile, String user, Duration timeout) {
    ManagedChannel channel = getManagedChannel(nodeAgent, true);
    try (OutputStream outputStream = new BufferedOutputStream(new FileOutputStream(outputFile))) {
      NodeAgentStub stub = NodeAgentGrpc.newStub(channel);
      String id = String.format("%s-%s", nodeAgent.getUuid(), outputFile);
      DownloadFileResponseObserver responseObserver =
          new DownloadFileResponseObserver(id, outputStream);
      DownloadFileRequest.Builder builder = DownloadFileRequest.newBuilder().setFilename(inputFile);
      if (StringUtils.isNotBlank(user)) {
        builder.setUser(user);
      }
      if (timeout != null && !timeout.isZero()) {
        stub = stub.withDeadlineAfter(timeout.toMillis(), TimeUnit.MILLISECONDS);
      }
      stub.downloadFile(builder.build(), responseObserver);
      responseObserver.waitFor();
    } catch (Exception e) {
      throw new RuntimeException(
          String.format(
              "Error in downloading file %s to %s. Error: %s",
              inputFile, outputFile, e.getMessage()),
          e);
    }
  }

  public void startUpgrade(NodeAgent nodeAgent, NodeAgentUpgradeParam param) {
    ManagedChannel channel = getManagedChannel(nodeAgent, true);
    NodeAgentBlockingStub stub = NodeAgentGrpc.newBlockingStub(channel);
    UpgradeInfo.Builder builder = UpgradeInfo.newBuilder();
    if (StringUtils.isNotBlank(param.certDir)) {
      builder.setCertDir(param.certDir);
    }
    if (param.packagePath != null) {
      builder.setPackagePath(param.packagePath.toString());
    }
    stub.update(
        UpdateRequest.newBuilder()
            .setState(State.UPGRADE.name())
            .setUpgradeInfo(builder.build())
            .build());
  }

  public String finalizeUpgrade(NodeAgent nodeAgent) {
    ManagedChannel channel = getManagedChannel(nodeAgent, true);
    NodeAgentBlockingStub stub = NodeAgentGrpc.newBlockingStub(channel);
    UpdateResponse response =
        stub.update(UpdateRequest.newBuilder().setState(State.UPGRADED.name()).build());
    return response.getHome();
  }

  public void abortTask(NodeAgent nodeAgent, String taskId) {
    ManagedChannel channel = getManagedChannel(nodeAgent, true);
    try {
      NodeAgentGrpc.newBlockingStub(channel)
          .abortTask(AbortTaskRequest.newBuilder().setTaskId(taskId).build());
    } catch (Exception e) {
      // Ignore error.
      log.error("Abort failed for task {} - {}", taskId, e.getMessage());
    }
  }

  public PreflightCheckOutput runPreflightCheck(
      NodeAgent nodeAgent, PreflightCheckInput input, String user) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setPreflightCheckInput(input)
            .setTaskId(UUID.randomUUID().toString());
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), PreflightCheckOutput.class);
  }

  public ConfigureServiceOutput runConfigureEarlyoom(
      NodeAgent nodeAgent, ConfigureServiceInput input, String user) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setTaskId(String.format("ConfigureEarlyoom-%s", UUID.randomUUID().toString()))
            .setConfigureServiceInput(input);
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), ConfigureServiceOutput.class);
  }

  public ServerControlOutput runServerControl(
      NodeAgent nodeAgent, ServerControlInput input, String user) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setServerControlInput(input)
            .setTaskId(UUID.randomUUID().toString());
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), ServerControlOutput.class);
  }

  public ConfigureServerOutput runConfigureServer(
      NodeAgent nodeAgent, ConfigureServerInput input, String user) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setConfigureServerInput(input)
            .setTaskId(UUID.randomUUID().toString());
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), ConfigureServerOutput.class);
  }

  public InstallSoftwareOutput runInstallSoftware(
      NodeAgent nodeAgent, InstallSoftwareInput input, String user) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setInstallSoftwareInput(input)
            .setTaskId(UUID.randomUUID().toString());
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), InstallSoftwareOutput.class);
  }

  public DownloadSoftwareOutput runDownloadSoftware(
      NodeAgent nodeAgent, DownloadSoftwareInput input, String user) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setDownloadSoftwareInput(input)
            .setTaskId(UUID.randomUUID().toString());
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), DownloadSoftwareOutput.class);
  }

  public InstallYbcOutput runInstallYbcSoftware(
      NodeAgent nodeAgent, InstallYbcInput input, String user) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setInstallYbcInput(input)
            .setTaskId(UUID.randomUUID().toString());
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), InstallYbcOutput.class);
  }

  public InstallOtelCollectorOutput runInstallOtelCollector(
      NodeAgent nodeAgent, InstallOtelCollectorInput input, String user) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setInstallOtelCollectorInput(input)
            .setTaskId(UUID.randomUUID().toString());
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), InstallOtelCollectorOutput.class);
  }

  public ConfigureCloudFederationOutput runConfigureCloudFederation(
      NodeAgent nodeAgent, ConfigureCloudFederationInput input, String user) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setConfigureCloudFederationInput(input)
            .setTaskId(UUID.randomUUID().toString());
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), ConfigureCloudFederationOutput.class);
  }

  public ConfigureNodeExporterOutput runConfigureNodeExporter(
      NodeAgent nodeAgent, ConfigureNodeExporterInput input, String user) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setConfigureNodeExporterInput(input)
            .setTaskId(UUID.randomUUID().toString());
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), ConfigureNodeExporterOutput.class);
  }

  public SetupCGroupOutput runSetupCGroupInput(
      NodeAgent nodeAgent, SetupCGroupInput input, String user) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setSetupCGroupInput(input)
            .setTaskId(UUID.randomUUID().toString());
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), SetupCGroupOutput.class);
  }

  public ServerGFlagsOutput runServerGFlags(
      NodeAgent nodeAgent, ServerGFlagsInput input, String user) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setTaskId(UUID.randomUUID().toString())
            .setServerGFlagsInput(input);
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), ServerGFlagsOutput.class);
  }

  public DestroyServerOutput runDestroyServer(
      NodeAgent nodeAgent, DestroyServerInput input, String user) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setTaskId(UUID.randomUUID().toString())
            .setDestroyServerInput(input);
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), DestroyServerOutput.class);
  }

  public YnpPreflightCheckOutput runYnpPreflightCheck(
      NodeAgent nodeAgent, YnpPreflightCheckInput input, String user) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setTaskId(UUID.randomUUID().toString())
            .setYnpPreflightCheckInput(input);
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), YnpPreflightCheckOutput.class);
  }

  public RotateSshKeyOutput runRotateSshKey(
      NodeAgent nodeAgent, RotateSshKeyInput input, String user) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setTaskId(UUID.randomUUID().toString())
            .setRotateSshKeyInput(input);
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), RotateSshKeyOutput.class);
  }

  public HealthCheckOutput runOrGetHealthCheck(
      NodeAgent nodeAgent, HealthCheckInput input, String user, Duration timeout) {
    SubmitTaskRequest.Builder builder =
        SubmitTaskRequest.newBuilder()
            .setTaskId(UUID.randomUUID().toString())
            .setHealthCheckInput(input);
    if (StringUtils.isNotBlank(user)) {
      builder.setUser(user);
    }
    return runAsyncTask(nodeAgent, builder.build(), HealthCheckOutput.class, timeout);
  }

  public synchronized void cleanupCachedClients() {
    try {
      cachedChannels.cleanUp();
      log.debug("Current cache size after cleanup: {}", getClientCacheSize());
    } catch (RuntimeException e) {
      log.error("Client cache cleanup failed {}", e.getMessage());
    }
  }

  // Common method to submit async task and wait for the result.
  private <T> T runAsyncTask(
      NodeAgent nodeAgent, SubmitTaskRequest request, Class<T> responseClass) {
    return runAsyncTask(
        nodeAgent,
        request,
        responseClass,
        confGetter.getGlobalConf(GlobalConfKeys.nodeAgentTokenLifetime));
  }

  // Common method to submit async task and wait for the result.
  private <T> T runAsyncTask(
      NodeAgent nodeAgent, SubmitTaskRequest request, Class<T> responseClass, Duration timeout) {
    Objects.requireNonNull(request.getTaskId(), "Task ID must be set");
    long pollDeadlineMs =
        confGetter.getGlobalConf(GlobalConfKeys.nodeAgentDescribePollDeadline).toMillis();
    int maxOutputBufferLines =
        confGetter.getGlobalConf(GlobalConfKeys.nodeAgentDescribeMaxOutputBufferLines);
    ManagedChannel channel = getManagedChannel(nodeAgent, true);
    SubmitTaskResponse response = NodeAgentGrpc.newBlockingStub(channel).submitTask(request);
    String taskId = response.getTaskId();
    String id = String.format("%s-%s", nodeAgent.getUuid(), taskId);
    DescribeTaskRequest describeTaskRequest =
        DescribeTaskRequest.newBuilder().setTaskId(taskId).build();
    CircularFifoQueue<String> outputBuffer = new CircularFifoQueue<>(maxOutputBufferLines);
    Deadline deadline = Deadline.after(timeout.toMillis(), TimeUnit.MILLISECONDS);
    int transientFailures = 0;
    while (true) {
      try {
        log.info("Describing task {}", taskId);
        // Reconnect to the node agent to get a new channel if necessary.
        channel = getManagedChannel(nodeAgent, true);
        NodeAgentStub stub = NodeAgentGrpc.newStub(channel);
        DescribeTaskResponseObserver<T> responseObserver =
            new DescribeTaskResponseObserver<>(id, responseClass, outputBuffer);
        stub.withDeadlineAfter(pollDeadlineMs, TimeUnit.MILLISECONDS)
            .describeTask(describeTaskRequest, responseObserver);
        return responseObserver.waitForResponse();
      } catch (StatusRuntimeException e) {
        if (deadline.isExpired()) {
          abortTask(nodeAgent, taskId);
          throw new RuntimeException(
              String.format(
                  "Task %s on node agent %s did not complete within %s",
                  taskId, nodeAgent, timeout),
              e);
        }
        if (e.getStatus().getCode() == Code.DEADLINE_EXCEEDED) {
          // Reset transient failure count.
          transientFailures = 0;
          log.info("Reconnecting to node agent {} to describe task {}", nodeAgent, taskId);
        } else if (e.getStatus().getCode() == Code.CANCELLED && !Context.current().isCancelled()) {
          // Client side did not cancel it.
          // Reset transient failure count.
          transientFailures = 0;
          log.info(
              "Cancelled by server. Reconnecting to node agent {} to describe task {}",
              nodeAgent,
              taskId);
        } else if (e.getStatus().getCode() == Code.UNAVAILABLE) {
          transientFailures++;
          if (transientFailures > MAX_TRANSIENT_FAILURES) {
            abortTask(nodeAgent, taskId);
            log.error(
                "Too many transient failures in describing task for node agent {} - {}",
                nodeAgent,
                e.getStatus());
            throw e;
          }
        } else {
          // Best effort to abort.
          abortTask(nodeAgent, taskId);
          log.error("Error in describing task for node agent {} - {}", nodeAgent, e.getStatus());
          throw e;
        }
      }
    }
  }

  private List<String> getBashCommand(List<String> command) {
    List<String> shellCommand = new ArrayList<>();
    // Same join as in rpc.py of node agent.
    shellCommand.add("bash");
    shellCommand.add("-c");
    shellCommand.add(
        command.stream()
            .map(part -> part.contains(" ") ? "'" + part + "'" : part)
            .collect(Collectors.joining(" ")));
    return shellCommand;
  }

  private long getClientCacheSize() {
    return cachedChannels.size();
  }
}
