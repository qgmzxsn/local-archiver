package app.localarchiver

object ArchiverNative {
    init {
        System.loadLibrary("archiver_jni")
    }

    external fun listArchive(archivePath: String, enginePath: String, password: CharArray?): String

    external fun extractArchive(
        archivePath: String,
        destPath: String,
        enginePath: String,
        password: CharArray?
    ): String
}
