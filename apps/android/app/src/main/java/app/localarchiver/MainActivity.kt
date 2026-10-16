package app.localarchiver

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.text.InputType
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File

/* Thin shell. It was not compiled in the phase 1 environment.
   The password field is not saved and is not written to preferences. */
class MainActivity : Activity() {
    private var archiveFile: File? = null
    private lateinit var password: EditText
    private lateinit var output: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 32, 32, 32)
        }
        val open = Button(this).apply { text = "打开压缩包" }
        val list = Button(this).apply { text = "列出" }
        val extract = Button(this).apply { text = "解压" }
        password = EditText(this).apply {
            hint = "需要密码时在这里输入。不会保存。"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            isSaveEnabled = false
        }
        output = TextView(this).apply {
            text = "第一阶段：ZIP、7z、tar 及其 gz/bz2/xz/zstd，RAR 只解压。"
        }
        root.addView(open)
        root.addView(password)
        root.addView(list)
        root.addView(extract)
        root.addView(output)
        setContentView(root)

        open.setOnClickListener {
            startActivityForResult(
                Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "*/*"
                },
                1
            )
        }
        list.setOnClickListener { runNative(extract = false) }
        extract.setOnClickListener { runNative(extract = true) }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != 1 || resultCode != RESULT_OK) return
        val uri: Uri = data?.data ?: return
        val dest = File(cacheDir, "opened-archive")
        contentResolver.openInputStream(uri)?.use { input ->
            dest.outputStream().use { input.copyTo(it) }
        }
        archiveFile = dest
        output.text = "已复制到应用缓存。密码不会写入设置。"
    }

    private fun runNative(extract: Boolean) {
        val file = archiveFile
        if (file == null) {
            output.text = "先打开一个压缩包。"
            return
        }
        val chars = password.text?.toString()?.toCharArray()
        password.text?.clear()
        try {
            val engine = applicationInfo.nativeLibraryDir + "/lib7z.so"
            output.text = if (extract) {
                val dest = getExternalFilesDir(null) ?: filesDir
                ArchiverNative.extractArchive(file.absolutePath, dest.absolutePath, engine, chars)
            } else {
                ArchiverNative.listArchive(file.absolutePath, engine, chars)
            }
        } finally {
            chars?.fill('\u0000')
        }
    }
}
