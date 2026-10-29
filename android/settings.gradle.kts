pluginManagement {
    repositories { google(); mavenCentral(); gradlePluginPortal() }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories { google(); mavenCentral() }
}
rootProject.name = "Ghosterm"
include(":app", ":bridge", ":terminal", ":domain", ":data")
project(":terminal").projectDir = file("../terminal/android")
