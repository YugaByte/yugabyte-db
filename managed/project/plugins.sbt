// The Play plugin
addSbtPlugin("com.typesafe.play" % "sbt-plugin" % "2.6.25")

// Play enhancer - this automatically generates getters/setters for public fields
// and rewrites accessors of these fields to use the getters/setters. Remove this
// plugin if you prefer not to have this feature, or disable on a per project
// basis using disablePlugins(PlayEnhancer) in your build.sbt
addSbtPlugin("com.typesafe.sbt" % "sbt-play-enhancer" % "1.2.2")

addSbtPlugin("com.frugalmechanic" % "fm-sbt-s3-resolver" % "0.4.0")

// ORM
addSbtPlugin("com.typesafe.sbt" % "sbt-play-ebean" % "4.1.4")

addSbtPlugin("com.typesafe.sbt" % "sbt-native-packager" % "1.3.4")

addSbtPlugin("com.github.sbt" % "sbt-jacoco" % "3.0.3")
