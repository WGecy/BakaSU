package org.bakasu.bakasu.data.shell

import android.content.Context
import android.net.Uri
import android.os.Environment
import android.os.SystemClock
import android.system.Os
import android.util.Log
import androidx.core.net.toUri
import com.topjohnwu.superuser.CallbackList
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ShellUtils
import com.topjohnwu.superuser.io.SuFile
import java.io.File
import java.io.IOException
import java.util.Properties
import java.util.concurrent.Executor
import java.util.zip.ZipFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.bakasu.bakasu.BuildConfig
import org.bakasu.bakasu.Natives
import org.bakasu.bakasu.R
import org.bakasu.bakasu.data.module.registry.DownloadVerificationException
import org.bakasu.bakasu.data.module.registry.RegistryDownloadStore
import org.bakasu.bakasu.data.module.registry.copyForInstall
import org.bakasu.bakasu.domain.model.BuiltinZygiskConfig
import org.bakasu.bakasu.domain.model.BuiltinZygiskStatus
import org.bakasu.bakasu.domain.model.FolkMountProvider
import org.bakasu.bakasu.domain.model.InjectedZygiskModule
import org.bakasu.bakasu.domain.model.InjectedZygiskProcess
import org.bakasu.bakasu.domain.model.LkmSelection
import org.bakasu.bakasu.domain.model.ZygiskImplementationInfo
import org.bakasu.bakasu.domain.model.ZygiskProcess
import org.bakasu.bakasu.domain.model.ZygoteAbiStatus
import org.json.JSONArray
import org.json.JSONObject

/**
 * @author weishu
 * @date 2023/1/1.
 */
class KsuCliRepository(context: Context) {
    /** One daemon answer plus when it was read; null means the daemon did not answer. */
    private class MountBackendAnswer(val provider: FolkMountProvider?, val at: Long)

    private companion object {
        const val TAG = "KsuCli"
        val MODULE_ID_REGEX = Regex("^[a-zA-Z][a-zA-Z0-9._-]+$")

        /** Must stay in sync with kernel/manager/manager_sign.h. */
        val KNOWN_OFFICIAL_SIGNATURES = setOf(
            "size: 0x377, hash: d3469712b6214462764a1d8d3e5cbe1d6819a0b629791b9f4101867821f1df64",
            "size: 0x32a, hash: 9abde89ef090198b0e5c5004110f223a3f59f5f238ac69770739feb9ef462d49",
        )

        /** Reuse window for the mount backend answer; it only changes across boots. */
        const val MOUNT_BACKEND_CACHE_MS = 5_000L
    }

    private val nativeLibraryDir = context.applicationInfo.nativeLibraryDir
    private val shellLock = Any()
    private var rootShell: Shell? = null
    private var globalMountShell: Shell? = null
    private val mountBackendLock = Any()
    private var mountBackendCache: MountBackendAnswer? = null

    private fun getNativeLibraryPath(name: String): String {
        val library = File(nativeLibraryDir, System.mapLibraryName(name))
        require(library.isFile) {
            "${library.name} was not found in the application native library path"
        }
        return library.absolutePath
    }

    fun getKsuDaemonPath(): String = getNativeLibraryPath("ksud")

    /** Shared shells belong to this repository; callers must not close them. */
    fun getRootShell(globalMnt: Boolean = false): Shell = synchronized(shellLock) {
        val cached = if (globalMnt) globalMountShell else rootShell
        if (cached != null && cached.isAlive) {
            cached
        } else {
            createRootShell(globalMnt).also {
                if (globalMnt) globalMountShell = it else rootShell = it
            }
        }
    }

    fun generateMainShellBuilder(): Shell.Builder {
        val builder = Shell.Builder.create()
        try {
            builder.setCommands(getKsuDaemonPath(), "debug", "su")
            builder.build().close()
        } catch (e: Throwable) {
            Log.w(TAG, "ksu failed: ", e)
            try {
                builder.setCommands("su")
                builder.build().close()
            } catch (e: Throwable) {
                Log.e(TAG, "su failed: ", e)
                builder.setCommands("sh")
                builder.build().close()
            }
        }

        return builder
    }

    inline fun <T> withNewRootShell(
        globalMnt: Boolean = false,
        block: Shell.() -> T,
    ): T = createRootShell(globalMnt).use(block)

    fun createRootShell(globalMnt: Boolean = false): Shell {
        Shell.enableVerboseLogging = BuildConfig.DEBUG
        val builder = Shell.Builder.create()
        return try {
            if (globalMnt) {
                builder.build(getKsuDaemonPath(), "debug", "su", "-g")
            } else {
                builder.build(getKsuDaemonPath(), "debug", "su")
            }
        } catch (e: Throwable) {
            Log.w(TAG, "ksu failed: ", e)
            try {
                if (globalMnt) {
                    builder.build("su", "-mm")
                } else {
                    builder.build("su")
                }
            } catch (e: Throwable) {
                Log.e(TAG, "su failed: ", e)
                builder.build("sh")
            }
        }
    }

    fun execKsud(args: String, newShell: Boolean = false, globalMnt: Boolean = false): Boolean = if (newShell) {
        withNewRootShell(globalMnt = globalMnt) {
            ShellUtils.fastCmdResult(this, "${getKsuDaemonPath()} $args")
        }
    } else {
        ShellUtils.fastCmdResult(getRootShell(globalMnt), "${getKsuDaemonPath()} $args")
    }

    internal fun shellQuote(value: String): String = "'${value.replace("'", "'\"'\"'")}'"

    data class DynamicManagerCliConfig(
        val size: Int = 0,
        val hash: String = "",
    ) {
        fun isValid(): Boolean = size > 0 && hash.length == 64
    }

    suspend fun getDynamicManagerConfig(): DynamicManagerCliConfig? = withContext(Dispatchers.IO) {
        val shell = getRootShell()
        val result = shell.newJob()
            .add("${getKsuDaemonPath()} kernel dynamic-manager get --internal true")
            .to(ArrayList<String>(), null)
            .exec()
        if (!result.isSuccess) return@withContext null

        runCatching {
            val obj = JSONObject(result.out.joinToString("\n"))
            DynamicManagerCliConfig(
                size = obj.optInt("size", 0),
                hash = obj.optString("hash", ""),
            )
        }.getOrNull()
    }

    fun setDynamicManager(size: Int, hash: String): Boolean {
        val result = execKsud("kernel dynamic-manager set $size $hash", true)
        Log.i(TAG, "set dynamic manager result: $result")
        return result
    }

    fun setDynamicManagerApk(apkPath: String): Boolean {
        val result = execKsud("kernel dynamic-manager set-apk ${shellQuote(apkPath)}", true)
        Log.i(TAG, "set dynamic manager apk result: $result")
        return result
    }

    fun clearDynamicManager(): Boolean {
        val result = execKsud("kernel dynamic-manager clear", true)
        Log.i(TAG, "clear dynamic manager result: $result")
        return result
    }

    suspend fun isOfficialSignature(packageResourcePath: String): Boolean = withContext(Dispatchers.IO) {
        val shell = getRootShell()
        val out = shell.newJob()
            .add("${getKsuDaemonPath()} debug get-sign ${shellQuote(packageResourcePath)}")
            .to(ArrayList<String>(), null).exec().out
        out.firstOrNull()?.trim().orEmpty() in KNOWN_OFFICIAL_SIGNATURES
    }

    suspend fun getFeatureStatus(feature: String): String = withContext(Dispatchers.IO) {
        val shell = getRootShell()
        val out = shell.newJob()
            .add("${getKsuDaemonPath()} feature check $feature").to(ArrayList<String>(), null)
            .exec().out
        out.firstOrNull()?.trim().orEmpty()
    }

    /** Runs `ksud mount <args>` and returns its stdout. Arguments are never user input. */
    suspend fun execMount(args: String): Result<String> = withContext(Dispatchers.IO) {
        runCatching {
            val shell = getRootShell()
            val result = shell.newJob()
                .add("${getKsuDaemonPath()} mount $args")
                .to(ArrayList<String>(), null)
                .exec()
            if (!result.isSuccess) {
                throw IllegalStateException(
                    (result.err + result.out).joinToString("\n").trim()
                        .ifBlank { "mount $args failed" },
                )
            }
            result.out.joinToString("\n")
        }
    }

    /**
     * Provider that mounted the modules this boot. The daemon's boot record wins; the
     * predicted provider only describes the next boot. Null until the daemon answers.
     */
    fun getMountProvider(): FolkMountProvider? {
        val now = SystemClock.elapsedRealtime()
        synchronized(mountBackendLock) {
            mountBackendCache?.takeIf { now - it.at < MOUNT_BACKEND_CACHE_MS }?.let { return it.provider }
        }
        val provider = runCatching {
            val raw = runCmd(getRootShell(), "${getKsuDaemonPath()} mount status --json")
            val payload = raw.lineSequence().firstOrNull { it.trimStart().startsWith("{") }
            val json = JSONObject(payload ?: return@runCatching null)
            if (json.optString("boot_result") == "skipped") {
                FolkMountProvider.NONE
            } else {
                FolkMountProvider.fromToken(json.optString("boot_provider").ifBlank { null })
                    ?: FolkMountProvider.fromToken(json.optString("next_provider").ifBlank { null })
            }
        }.getOrNull()
        synchronized(mountBackendLock) { mountBackendCache = MountBackendAnswer(provider, now) }
        return provider
    }

    /** Whether a built-in backend (Folk Mount or NoMount) mounted the modules this boot. */
    fun isBuiltInMountEngine(): Boolean = when (getMountProvider()) {
        FolkMountProvider.BUILTIN, FolkMountProvider.NOMOUNT -> true
        FolkMountProvider.METAMODULE, FolkMountProvider.NONE, null -> false
    }

    /** Runs a ksud command and returns the whole [Shell.Result]; quote any user input. */
    suspend fun execKsudResult(args: String): Shell.Result = withContext(Dispatchers.IO) {
        getRootShell().newJob()
            .add("${getKsuDaemonPath()} $args")
            .to(ArrayList<String>(), ArrayList<String>())
            .exec()
    }

    /** Runs a raw root shell command and returns stdout, or null on failure. */
    suspend fun exec(cmd: String): String? = withContext(Dispatchers.IO) {
        val result = getRootShell().newJob()
            .add(cmd)
            .to(ArrayList<String>(), null)
            .exec()
        if (!result.isSuccess) null else result.out.joinToString("\n")
    }

    suspend fun getFeaturePersistValue(feature: String): Long? = readFeatureValue("--config $feature")

    // Current value in kernel
    suspend fun getFeatureValue(feature: String): Long? = readFeatureValue(feature)

    private suspend fun readFeatureValue(args: String): Long? = withContext(Dispatchers.IO) {
        val shell = getRootShell()
        val out = shell.newJob()
            .add("${getKsuDaemonPath()} feature get $args")
            .to(ArrayList<String>(), null)
            .exec().out
        val valueLine =
            out.firstOrNull { it.trim().startsWith("Value:") } ?: return@withContext null
        valueLine.substringAfter("Value:").trim().toLongOrNull()
    }

    fun install() {
        val start = SystemClock.elapsedRealtime()
        val libadbroot = getNativeLibraryPath("adbroot")
        val result = execKsud("install --libadbroot $libadbroot", true)
        Log.w(TAG, "install result: $result, cost: ${SystemClock.elapsedRealtime() - start}ms")
    }

    fun listModules(): String {
        val shell = getRootShell()

        val out = shell.newJob()
            .add("${getKsuDaemonPath()} module list").to(ArrayList(), null).exec().out
        return out.joinToString("\n").ifBlank { "[]" }
    }

    fun getModuleCount(): Int {
        val result = listModules()
        runCatching {
            val array = JSONArray(result)
            return array.length()
        }.getOrElse { return 0 }
    }

    fun getSuperuserCount(): Int = Natives.getSuperuserCount()

    fun toggleModule(id: String, enable: Boolean): Boolean {
        val cmd = if (enable) {
            "module enable $id"
        } else {
            "module disable $id"
        }
        val result = execKsud(cmd, true)
        Log.i(TAG, "$cmd result: $result")
        return result
    }

    fun uninstallModule(id: String): Boolean {
        val cmd = "module uninstall $id"
        val result = execKsud(cmd, true)
        Log.i(TAG, "uninstall module $id result: $result")
        return result
    }

    fun undoUninstallModule(id: String): Boolean {
        val cmd = "module undo-uninstall $id"
        val result = execKsud(cmd, true)
        Log.i(TAG, "undo uninstall module $id result: $result")
        return result
    }

    private fun flashWithIO(
        cmd: String,
        onStdout: (String) -> Unit,
        onStderr: (String) -> Unit,
    ): Shell.Result {
        val stdoutCallback: CallbackList<String?> = object : CallbackList<String?>() {
            override fun onAddElement(s: String?) {
                onStdout(s ?: "")
            }
        }

        val stderrCallback: CallbackList<String?> = object : CallbackList<String?>() {
            override fun onAddElement(s: String?) {
                onStderr(s ?: "")
            }
        }

        return withNewRootShell {
            newJob().add(cmd).to(stdoutCallback, stderrCallback).exec()
        }
    }

    fun flashModule(
        context: Context,
        uri: Uri,
        onFinish: (Boolean, Int) -> Unit,
        onStdout: (String) -> Unit,
        onStderr: (String) -> Unit,
    ): Boolean {
        val root = File(context.filesDir, RegistryDownloadStore.DIR_NAME)
        val expected = if (uri.scheme == "file") uri.path?.let { RegistryDownloadStore.expectedFor(root, File(it)) } else null
        val file = File.createTempFile("module-", ".zip", context.cacheDir)
        try {
            try {
                val input = context.contentResolver.openInputStream(uri) ?: throw IOException("Cannot open module")
                input.use { copyForInstall(it, file, expected) }
            } catch (error: IOException) {
                if (error is DownloadVerificationException) uri.path?.let { RegistryDownloadStore.delete(root, File(it)) }
                onStderr(context.getString(if (error is DownloadVerificationException) R.string.repo_install_verify_failed else R.string.repo_error_module_archive))
                onFinish(false, 1)
                return false
            }
            val cmd = "module install ${file.absolutePath}"
            val result = flashWithIO("${getKsuDaemonPath()} $cmd", onStdout, onStderr)
            Log.i("KernelSU", "install module $uri result: $result")

            onFinish(result.isSuccess && moduleNeedsReboot(file), result.code)
            return result.isSuccess
        } finally {
            file.delete()
        }
    }

    private fun moduleNeedsReboot(zip: File): Boolean {
        val id = runCatching {
            ZipFile(zip).use { archive ->
                val entry = archive.getEntry("module.prop") ?: return@use null
                archive.getInputStream(entry).bufferedReader().useLines { lines ->
                    lines.map { it.trim() }.firstOrNull { it.startsWith("id=") }
                        ?.substringAfter('=')?.trim()
                }
            }
        }.getOrNull()
        if (id == null || !MODULE_ID_REGEX.matches(id)) return true
        val dir = shellQuote("/data/adb/modules/$id")
        return runCatching {
            !ShellUtils.fastCmdResult(getRootShell(), "[ -d $dir ] && [ ! -e $dir/update ]")
        }.getOrDefault(true)
    }

    fun flashAnyKernel(
        zipFile: File,
        slot: String?,
        onStdout: (String) -> Unit,
        onStderr: (String) -> Unit,
    ): Boolean {
        val command = buildString {
            append("${getKsuDaemonPath()} anykernel3 ${shellQuote(zipFile.absolutePath)}")
            slot?.let {
                append(" --slot ${shellQuote(it)}")
            }
        }
        val result = flashWithIO(command, onStdout, onStderr)
        Log.i(TAG, "AnyKernel3 flash result: ${result.isSuccess}, code: ${result.code}")
        return result.isSuccess
    }

    fun runModuleAction(
        moduleId: String,
        onStdout: (String) -> Unit,
        onStderr: (String) -> Unit,
    ): Boolean {
        val stdoutCallback: CallbackList<String?> = object : CallbackList<String?>() {
            override fun onAddElement(s: String?) {
                onStdout(s ?: "")
            }
        }

        val stderrCallback: CallbackList<String?> = object : CallbackList<String?>() {
            override fun onAddElement(s: String?) {
                onStderr(s ?: "")
            }
        }

        val result = withNewRootShell(true) {
            newJob().add("${getKsuDaemonPath()} module action $moduleId")
                .to(stdoutCallback, stderrCallback).exec()
        }

        Log.i("KernelSU", "Module runAction result: $result")

        return result.isSuccess
    }

    /** A dedicated shell keeps the script out of the manager's command stream. */
    fun runOneshotScript(command: String, onStdout: (String) -> Unit, onStderr: (String) -> Unit): Int = withNewRootShell(globalMnt = true) {
        check(isRoot) { "Root access is unavailable" }
        val direct = Executor { it.run() }
        val stdout = object : CallbackList<String?>(direct, null) {
            override fun onAddElement(s: String?) = onStdout(s.orEmpty())
        }
        val stderr = object : CallbackList<String?>(direct, null) {
            override fun onAddElement(s: String?) = onStderr(s.orEmpty())
        }
        newJob().add(command).to(stdout, stderr).exec().code
    }

    fun removeScriptDirectory(path: String): Boolean = withNewRootShell {
        isRoot && ShellUtils.fastCmdResult(this, "rm -rf -- ${shellQuote(path)}")
    }

    fun restoreBoot(
        onFinish: (Boolean, Int) -> Unit,
        onStdout: (String) -> Unit,
        onStderr: (String) -> Unit,
    ): Boolean {
        val result = flashWithIO(
            "${getKsuDaemonPath()} boot-restore -f",
            onStdout,
            onStderr,
        )
        onFinish(result.isSuccess, result.code)
        return result.isSuccess
    }

    fun uninstallPermanently(
        onFinish: (Boolean, Int) -> Unit,
        onStdout: (String) -> Unit,
        onStderr: (String) -> Unit,
    ): Boolean {
        val result =
            flashWithIO(
                "${getKsuDaemonPath()} uninstall --package-name ${BuildConfig.APPLICATION_ID}",
                onStdout,
                onStderr,
            )
        onFinish(result.isSuccess, result.code)
        return result.isSuccess
    }

    fun installBoot(
        context: Context,
        bootUri: Uri?,
        lkm: LkmSelection,
        ota: Boolean,
        partition: String?,
        allowShell: Boolean = false,
        enableAdb: Boolean = false,
        forceBackup: Boolean = false,
        onFinish: (Boolean, Int) -> Unit,
        onStdout: (String) -> Unit,
        onStderr: (String) -> Unit,
    ): Boolean {
        val resolver = context.contentResolver

        val bootFile = bootUri?.let { uri ->
            with(resolver.openInputStream(uri)) {
                val bootFile = File(context.cacheDir, "boot.img")
                bootFile.outputStream().use { output ->
                    this?.copyTo(output)
                }

                bootFile
            }
        }

        var cmd = "boot-patch"

        cmd += if (bootFile == null) {
            // no boot.img, use -f to flash
            " -f"
        } else {
            " -b ${bootFile.absolutePath}"
        }

        if (ota) {
            cmd += " -u"
        }

        var lkmFile: File? = null
        when (lkm) {
            is LkmSelection.LkmUri -> {
                lkmFile = with(resolver.openInputStream(lkm.uri.toUri())) {
                    val file = File(context.cacheDir, "kernelsu-tmp-lkm.ko")
                    file.outputStream().use { output ->
                        this?.copyTo(output)
                    }

                    file
                }
                cmd += " -m ${lkmFile.absolutePath}"
            }

            is LkmSelection.KmiString -> {
                cmd += " --kmi ${lkm.value}"
            }

            LkmSelection.KmiNone -> {
                // do nothing
            }
        }

        // output dir
        if (bootFile != null) {
            val downloadsDir =
                Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
            cmd += " -o $downloadsDir"
        }

        partition?.let { part ->
            cmd += " --partition $part"
        }

        if (allowShell) {
            cmd += " --allow-shell"
        }
        if (enableAdb) {
            cmd += " --enable-adbd"
        }
        if (forceBackup) {
            cmd += " --backup"
        }

        val result = flashWithIO("${getKsuDaemonPath()} $cmd", onStdout, onStderr)
        Log.i("KernelSU", "install boot result: ${result.isSuccess}")

        bootFile?.delete()
        lkmFile?.delete()

        // if boot uri is empty, it is direct install, when success, we should show reboot button
        onFinish(bootUri == null && result.isSuccess, result.code)

        if (bootUri == null && result.isSuccess) {
            install()
        }

        return result.isSuccess
    }

    fun reboot(reason: String = "") {
        if (reason == "soft_reboot") {
            execKsud("soft-reboot", newShell = true, globalMnt = true)
            return
        }
        val shell = getRootShell()
        if (reason == "recovery") {
            // KEYCODE_POWER = 26, hide incorrect "Factory data reset" message
            ShellUtils.fastCmd(shell, "/system/bin/input keyevent 26")
        }
        ShellUtils.fastCmd(
            shell,
            "/system/bin/svc power reboot $reason || /system/bin/reboot $reason",
        )
    }

    fun rootAvailable(): Boolean {
        val shell = getRootShell()
        return shell.isRoot
    }

    suspend fun getCurrentKmi(): String = withContext(Dispatchers.IO) {
        val shell = getRootShell()
        val cmd = "boot-info current-kmi"
        ShellUtils.fastCmd(shell, "${getKsuDaemonPath()} $cmd")
    }

    suspend fun getSupportedKmis(): List<String> = withContext(Dispatchers.IO) {
        val shell = getRootShell()
        val cmd = "boot-info supported-kmis"
        val out = shell.newJob().add("${getKsuDaemonPath()} $cmd").to(ArrayList(), null).exec().out
        out.filter { it.isNotBlank() }.map { it.trim() }
    }

    suspend fun isAbDevice(): Boolean = withContext(Dispatchers.IO) {
        val shell = getRootShell()
        val cmd = "boot-info is-ab-device"
        ShellUtils.fastCmd(shell, "${getKsuDaemonPath()} $cmd").trim().toBoolean()
    }

    suspend fun getDefaultPartition(): String = withContext(Dispatchers.IO) {
        val shell = getRootShell()
        if (shell.isRoot) {
            val cmd = "boot-info default-partition"
            ShellUtils.fastCmd(shell, "${getKsuDaemonPath()} $cmd").trim()
        } else {
            if (!Os.uname().release.contains("android12-")) "init_boot" else "boot"
        }
    }

    suspend fun getSlotSuffix(ota: Boolean): String = withContext(Dispatchers.IO) {
        val shell = getRootShell()
        val cmd = if (ota) {
            "boot-info slot-suffix --ota"
        } else {
            "boot-info slot-suffix"
        }
        ShellUtils.fastCmd(shell, "${getKsuDaemonPath()} $cmd").trim()
    }

    suspend fun getAvailablePartitions(): List<String> = withContext(Dispatchers.IO) {
        val shell = getRootShell()
        val cmd = "boot-info available-partitions"
        val out = shell.newJob().add("${getKsuDaemonPath()} $cmd").to(ArrayList(), null).exec().out
        out.filter { it.isNotBlank() }.map { it.trim() }
    }

    fun hasMagisk(): Boolean {
        val shell = getRootShell(true)
        val result = shell.newJob().add("which magisk").exec()
        Log.i(TAG, "has magisk: ${result.isSuccess}")
        return result.isSuccess
    }

    fun isSepolicyValid(rules: String?): Boolean {
        if (rules == null) {
            return true
        }
        val shell = getRootShell()
        val result =
            shell.newJob().add("${getKsuDaemonPath()} sepolicy check '$rules'")
                .to(ArrayList(), null)
                .exec()
        return result.isSuccess
    }

    fun getSepolicy(pkg: String): String {
        val shell = getRootShell()
        val result =
            shell.newJob().add("${getKsuDaemonPath()} profile get-sepolicy $pkg")
                .to(ArrayList(), null)
                .exec()
        Log.i(TAG, "code: ${result.code}, out: ${result.out}, err: ${result.err}")
        return result.out.joinToString("\n")
    }

    fun setSepolicy(pkg: String, rules: String): Boolean {
        val shell = getRootShell()
        val result = shell.newJob().add("${getKsuDaemonPath()} profile set-sepolicy $pkg '$rules'")
            .to(ArrayList(), null).exec()
        Log.i(TAG, "set sepolicy result: ${result.code}")
        return result.isSuccess
    }

    fun listAppProfileTemplates(): List<String> {
        val shell = getRootShell()
        return shell.newJob().add("${getKsuDaemonPath()} profile list-templates")
            .to(ArrayList(), null)
            .exec().out
    }

    fun getAppProfileTemplate(id: String): String {
        val shell = getRootShell()
        return shell.newJob().add("${getKsuDaemonPath()} profile get-template '$id'")
            .to(ArrayList(), null).exec().out.joinToString("\n")
    }

    fun setAppProfileTemplate(id: String, template: String): Boolean {
        val shell = getRootShell()
        val escapedTemplate = template.replace("\"", "\\\"")
        val cmd = """${getKsuDaemonPath()} profile set-template "$id" "$escapedTemplate""""
        return shell.newJob().add(cmd)
            .to(ArrayList(), null).exec().isSuccess
    }

    fun deleteAppProfileTemplate(id: String): Boolean {
        val shell = getRootShell()
        return shell.newJob().add("${getKsuDaemonPath()} profile delete-template '$id'")
            .to(ArrayList(), null).exec().isSuccess
    }

    fun runCmd(shell: Shell, cmd: String): String = shell.newJob()
        .add(cmd)
        .to(mutableListOf<String>(), null)
        .exec().out
        .joinToString("\n")

    fun forceStopApp(packageName: String) {
        val shell = getRootShell()
        val result = shell.newJob().add("am force-stop $packageName").exec()
        Log.i(TAG, "force stop $packageName result: $result")
    }

    fun launchApp(packageName: String) {
        val shell = getRootShell()
        val result =
            shell.newJob()
                .add("cmd package resolve-activity --brief $packageName | tail -n 1 | xargs cmd activity start-activity -n")
                .exec()
        Log.i(TAG, "launch $packageName result: $result")
    }

    fun restartApp(packageName: String) {
        forceStopApp(packageName)
        launchApp(packageName)
    }

    fun getMetaModuleImplement(): String {
        try {
            val metaModuleProp = SuFile.open("/data/adb/metamodule/module.prop")
            if (!metaModuleProp.isFile) {
                Log.i(TAG, "Meta module implement: None")
                return "None"
            }

            val prop = Properties()
            prop.load(metaModuleProp.newInputStream())

            val name = prop.getProperty("name")
            Log.i(TAG, "Meta module implement: $name")
            return name
        } catch (_: Throwable) {
            Log.i(TAG, "Meta module implement: None")
            return "None"
        }
    }

    /** Runs `ksud zygisk ...` and returns stdout, or null when the daemon refused. */
    private fun runZygisk(args: String): String? {
        val shell = getRootShell()
        val result = shell.newJob()
            .add("${getKsuDaemonPath()} zygisk $args")
            .to(ArrayList(), ArrayList())
            .exec()
        if (!result.isSuccess) {
            Log.w(TAG, "ksud zygisk $args failed: ${result.err.joinToString("\n")}")
            return null
        }
        return result.out.joinToString("\n").trim()
    }

    /** Built-in Zygisk (NyaZygisk) runtime state; a failed read reports "nothing enabled". */
    fun getBuiltinZygiskStatus(): BuiltinZygiskStatus {
        val payload = runZygisk("status") ?: return BuiltinZygiskStatus()
        return runCatching {
            val json = JSONObject(payload.lineSequence().last { it.isNotBlank() })
            BuiltinZygiskStatus(
                enabled = json.optBoolean("enabled"),
                monitorRunning = json.optBoolean("monitor_running"),
                daemonRunning = json.optBoolean("daemon_running"),
                zygoteInjected = json.optBoolean("zygote_injected"),
                fullyActive = json.optBoolean("fully_active"),
                zygotes = json.optJSONArray("zygotes").toZygoteAbiStatuses(),
                monitor = json.optJSONObject("monitor")?.toZygiskProcess(),
                daemons = json.optJSONArray("daemons").toZygiskProcesses(),
                version = json.optString("version"),
                versionCode = json.optString("version_code"),
            )
        }.getOrElse {
            Log.w(TAG, "cannot parse built-in Zygisk status", it)
            BuiltinZygiskStatus()
        }
    }

    /** NyaZygisk options (`config.prop`); unreadable options fall back to the shipped defaults. */
    fun getBuiltinZygiskConfig(): BuiltinZygiskConfig {
        val payload = runZygisk("config get") ?: return BuiltinZygiskConfig()
        return runCatching {
            val json = JSONObject(payload.lineSequence().last { it.isNotBlank() })
            BuiltinZygiskConfig(
                anonymousMemory = json.optString("anonymous_memory") == "1",
                customLinker = json.optString("custom_linker") == "1",
                pltEngine = json.optString("plt_engine")
                    .takeIf { it.isNotBlank() }
                    ?: BuiltinZygiskConfig.PLT_ENGINE_YUKIPLT,
            )
        }.getOrElse {
            Log.w(TAG, "cannot parse built-in Zygisk options", it)
            BuiltinZygiskConfig()
        }
    }

    /** Writes one NyaZygisk option; the daemon validates the key and the value. */
    fun setBuiltinZygiskConfig(key: String, value: String): Boolean = runZygisk("config set ${shellQuote(key)} ${shellQuote(value)}") == "true"

    /** Every Zygisk module the built-in is serving, with the processes it is attached to. */
    fun getInjectedZygiskModules(): List<InjectedZygiskModule> {
        val payload = runZygisk("modules") ?: return emptyList()
        return runCatching {
            val array = JSONObject(payload.lineSequence().last { it.isNotBlank() })
                .optJSONArray("modules")
                ?: JSONArray()
            (0 until array.length()).map { index ->
                val value = array.getJSONObject(index)
                InjectedZygiskModule(
                    id = value.optString("id"),
                    name = value.optString("name"),
                    version = value.optString("version"),
                    type = value.optString("type"),
                    abi = value.optJSONArray("abi").toStringList(),
                    targets = value.optJSONArray("targets").toStringList(),
                    companion = value.optBoolean("companion"),
                    state = value.optString("state")
                        .takeIf { it.isNotBlank() }
                        ?: InjectedZygiskModule.STATE_UNKNOWN,
                    processes = value.optJSONArray("processes").toInjectedZygiskProcesses(),
                )
            }
        }.getOrElse {
            Log.w(TAG, "cannot parse injected Zygisk modules", it)
            emptyList()
        }
    }

    /** The Zygisk implementation this system runs, or the one it would run after a reboot. */
    fun getZygiskImplementationInfo(): ZygiskImplementationInfo {
        val payload = runZygisk("info") ?: return ZygiskImplementationInfo()
        return runCatching {
            val json = JSONObject(payload.lineSequence().last { it.isNotBlank() })
            ZygiskImplementationInfo(
                state = json.optString("state").takeIf { it.isNotBlank() }
                    ?: ZygiskImplementationInfo.STATE_NONE,
                id = json.optString("id"),
                name = json.optString("name"),
                version = json.optString("version"),
                providers = json.optJSONArray("providers").toStringList(),
            )
        }.getOrElse {
            Log.w(TAG, "cannot parse the Zygisk implementation", it)
            ZygiskImplementationInfo()
        }
    }

    /** Turns the built-in Zygisk on or off; turning it on hands the running system over. */
    fun setBuiltinZygiskEnabled(enabled: Boolean): Boolean = runZygisk(if (enabled) "on" else "off") == "true"

    /** Switches to the built-in Zygisk or to an installed provider module, live. */
    fun switchZygiskImplementation(target: String): Boolean = runZygisk("switch ${shellQuote(target)}") == "true"

    /** Reloads the built-in Zygisk monitor so the running zygotes get re-injected. */
    fun rearmBuiltinZygisk(): Boolean = runZygisk("rearm") == "true"

    fun getZygiskImplement(): String {
        // ksud knows both implementations: the built-in one and an installed
        // provider module. Ask it first so the home row names whichever is
        // actually active instead of only the legacy module ids below.
        val info = getZygiskImplementationInfo()
        if (info.name.isNotBlank() && info.state != ZygiskImplementationInfo.STATE_NONE) {
            val label = listOf(info.name, info.version).filter { it.isNotBlank() }.joinToString(" ")
            Log.i(TAG, "Zygisk implement: $label (${info.state})")
            return label
        }

        val zygiskModuleIds = listOf(
            "zygisksu",
            "rezygisk",
        )

        for (moduleId in zygiskModuleIds) {
            if (SuFile.open("/data/adb/modules/$moduleId/disable").isFile || SuFile.open("/data/adb/modules/$moduleId/remove").isFile) continue

            val propFile = SuFile.open("/data/adb/modules/$moduleId/module.prop")
            if (!propFile.isFile) continue

            val prop = Properties()
            prop.load(propFile.newInputStream())

            val name = prop.getProperty("name")
            Log.i(TAG, "Zygisk implement: $name")
            return name
        }

        Log.i(TAG, "Zygisk implement: None")
        return "None"
    }

    fun addKernelUmountPath(path: String, flags: Int): Boolean {
        val shell = getRootShell()
        val flagsArg = if (flags >= 0) "--flags $flags" else ""
        val cmd = "${getKsuDaemonPath()} kernel umount add $path $flagsArg"
        val result = ShellUtils.fastCmdResult(shell, cmd)
        Log.i(TAG, "add umount path $path result: $result")
        return result
    }

    fun removeKernelUmountPath(path: String): Boolean {
        val shell = getRootShell()
        val cmd = "${getKsuDaemonPath()} kernel umount del $path"
        val result = ShellUtils.fastCmdResult(shell, cmd)
        Log.i(TAG, "remove umount path $path result: $result")
        return result
    }

    fun listKernelUmountPaths(): String {
        val shell = getRootShell()
        val cmd = "${getKsuDaemonPath()} kernel umount list"
        return try {
            runCmd(shell, cmd).trim()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to list umount paths", e)
            ""
        }
    }

    fun addUmountConfigUmountPath(path: String, flags: Int): Boolean {
        val shell = getRootShell()
        val flagsArg = if (flags >= 0) "--flags $flags" else ""
        val cmd = "${getKsuDaemonPath()} umount-config add $path $flagsArg"
        val result = ShellUtils.fastCmdResult(shell, cmd)
        Log.i(TAG, "add umount path $path result: $result")
        return result
    }

    fun removeUmountConfigUmountPath(path: String): Boolean {
        val shell = getRootShell()
        val cmd = "${getKsuDaemonPath()} umount-config del $path"
        val result = ShellUtils.fastCmdResult(shell, cmd)
        Log.i(TAG, "remove umount path $path result: $result")
        return result
    }

    fun listUmountConfigUmountPaths(): String {
        val shell = getRootShell()
        val cmd = "${getKsuDaemonPath()} umount-config list"
        return try {
            runCmd(shell, cmd).trim()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to list umount paths", e)
            ""
        }
    }
}

private fun JSONObject.toZygiskProcess(): ZygiskProcess = ZygiskProcess(
    name = optString("name"),
    pid = optInt("pid"),
)

private fun JSONArray?.toZygiskProcesses(): List<ZygiskProcess> {
    if (this == null) return emptyList()
    return (0 until length()).map { getJSONObject(it).toZygiskProcess() }
}

private fun JSONArray?.toZygoteAbiStatuses(): List<ZygoteAbiStatus> {
    if (this == null) return emptyList()
    return (0 until length()).map { index ->
        val value = getJSONObject(index)
        ZygoteAbiStatus(abi = value.optString("abi"), status = value.optString("status"))
    }
}

private fun JSONArray?.toStringList(): List<String> {
    if (this == null) return emptyList()
    return (0 until length()).map { optString(it) }
}

private fun JSONArray?.toInjectedZygiskProcesses(): List<InjectedZygiskProcess> {
    if (this == null) return emptyList()
    return (0 until length()).map { index ->
        val value = getJSONObject(index)
        InjectedZygiskProcess(
            name = value.optString("name"),
            pid = value.optInt("pid"),
            kind = value.optString("kind"),
        )
    }
}
