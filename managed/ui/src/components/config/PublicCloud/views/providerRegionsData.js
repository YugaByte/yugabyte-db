// These should match the metadata in devops under opscli/ybops/data/aws-metadata.yml
export const regionsData = [
  {
    destVpcRegion: 'ap-northeast-1',
    zones: ['ap-northeast-1a', 'ap-northeast-1c', 'ap-northeast-1d']
  },
  // This is disabled in devops, so keep it disabled here as well. Pending on fetching data from a
  // YW API: ENG-4225
  // {
  //   destVpcRegion: 'ap-northeast-2',
  //   zones: ['ap-northeast-2a', 'ap-northeast-2c']
  // },
  {
    destVpcRegion: 'ap-south-1',
    zones: ['ap-south-1a', 'ap-south-1b']
  },
  {
    destVpcRegion: 'ap-southeast-1',
    zones: ['ap-southeast-1a', 'ap-southeast-1b', 'ap-southeast-1c']
  },
  {
    destVpcRegion: 'ap-southeast-2',
    zones: ['ap-southeast-2a', 'ap-southeast-2b', 'ap-southeast-2c']
  },
  {
    destVpcRegion: 'ca-central-1',
    zones: ['ca-central-1a', 'ca-central-1b']
  },
  {
    destVpcRegion: 'eu-central-1',
    zones: ['eu-central-1a', 'eu-central-1b', 'eu-central-1c']
  },
  {
    destVpcRegion: 'eu-west-1',
    zones: ['eu-west-1a', 'eu-west-1b', 'eu-west-1c']
  },
  {
    destVpcRegion: 'eu-west-2',
    zones: ['eu-west-2a', 'eu-west-2b', 'eu-west-2c']
  },
  {
    destVpcRegion: 'eu-west-3',
    zones: ['eu-west-3a', 'eu-west-3b', 'eu-west-3c']
  },
  {
    destVpcRegion: 'sa-east-1',
    zones: ['sa-east-1a', 'sa-east-1b', 'sa-east-1c']
  },
  {
    destVpcRegion: 'us-east-1',
    zones: ['us-east-1a', 'us-east-1b', 'us-east-1c', 'us-east-1d', 'us-east-1e', 'us-east-1f']
  },
  {
    destVpcRegion: 'us-east-2',
    zones: ['us-east-2a', 'us-east-2b', 'us-east-2c']
  },
  {
    destVpcRegion: 'us-west-1',
    zones: ['us-west-1a', 'us-west-1b']
  },
  {
    destVpcRegion: 'us-west-2',
    zones: ['us-west-2a', 'us-west-2b', 'us-west-2c']
  }
];
