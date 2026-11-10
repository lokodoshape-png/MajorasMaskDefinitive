
package com.dishii.mm;
import org.libsdl.app.SDLActivity;

import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.Executors;

import android.Manifest;
import android.content.pm.PackageManager;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import android.os.Build;
import android.widget.Toast;

import android.util.Log;

import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyEvent;
import android.hardware.input.InputManager;

//This class is the main SDLActivity and just sets up a bunch of default files and the input overlay
public class MainActivity extends SDLActivity{

    SharedPreferences preferences;
    // Own data folder, so this build installs beside the original 2Ship (which uses /2S2H) and starts
    // from scratch: nothing is read from or written to the original's folder. Must match
    // libultraship's Context.cpp, which the Android workflow patches to the same name.
    static final String DATA_FOLDER = "Majoras Mask Android Definitive Edition";
    // Data folder of the builds before the rename; its files are moved into DATA_FOLDER.
    static final String OLD_DATA_FOLDER = "2S2H-MM3D";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        startStartupLog();
        preferences = getSharedPreferences("com.dishii.mm.prefs",Context.MODE_PRIVATE);

        // The activity is set up first (SDL's layout, the touch controls); the game's thread does not start the
        // game before the files are ready (setupGate), whatever the order the permission answers come back in.
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries) {
            setupGate.cancel("bibliotecas nativas não carregaram");
            return; // SDLActivity already shows the error and closes
        }

        setupControllerOverlay();
        attachController();
        // The virtual triggers start centred, which reads as half pressed.
        setButton(ControllerButtons.AXIS_LT, false);
        setButton(ControllerButtons.AXIS_RT, false);

        // Storage access first, then the port's files (onStorageReady). The opening video goes over everything,
        // including the touch controls, while the game boots under it. After an update the setup question comes
        // first; the video starts once it is answered. Without the game's files (mm.o2r), the preparation screen
        // comes first and the opening plays after it.
        beginStartup();
    }

    // ----- The preparation screen (SetupOverlay.java; the game's side is mm/2s2h/Extractor/SetupScreen.cpp)

    // The opening waits for the preparation screen: it starts when the game closes that screen (setupFinish).
    private boolean introWaitsForSetup = false;
    private boolean introPausedForSetup = false;

    static boolean gameFilesMissing() {
        File mm = new File(new File(Environment.getExternalStorageDirectory(), DATA_FOLDER), "mm.o2r");
        return !mm.isFile() || mm.length() == 0;
    }

    private void startIntroOrWaitForSetup() {
        if (gameFilesMissing()) {
            startupLog("Sem mm.o2r: a tela de preparação vem antes da abertura");
            introWaitsForSetup = true;
            SetupOverlay.progress(this, "Preparando o jogo", "Um momento...", -1f);
        } else {
            startIntro();
        }
    }

    void onSetupOverlayShown() {
        if (introActive()) {
            intro.pause();
            introPausedForSetup = true;
        }
    }

    void onSetupOverlayHidden() {
        if (introPausedForSetup && introActive()) {
            intro.resume();
        }
        introPausedForSetup = false;
    }

    // From the game: a question on the preparation screen. Waits for the answer (the game's thread, never the UI's).
    public static int setupAsk(String title, String text, String[] buttons, int def, int cancel) {
        final MainActivity activity = (MainActivity) mSingleton;
        if (activity == null) {
            return cancel;
        }
        startupLog("Tela de preparação: " + title);
        int answer = SetupOverlay.askAndWait(activity, title, text, buttons, def, cancel);
        startupLog("Tela de preparação: resposta " + answer);
        return answer;
    }

    public static void setupProgress(String title, String text, float fraction) {
        final MainActivity activity = (MainActivity) mSingleton;
        if (activity != null) {
            activity.runOnUiThread(() -> SetupOverlay.progress(activity, title, text, fraction));
        }
    }

    // From the game, before its own screen starts: the preparation screen closes and the opening plays.
    public static void setupFinish() {
        final MainActivity activity = (MainActivity) mSingleton;
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(() -> {
            SetupOverlay.hide(activity);
            if (activity.introWaitsForSetup) {
                activity.introWaitsForSetup = false;
                activity.startIntro();
            }
        });
    }

    // The setup question ("Configuração inicial") is on screen: the opening waits for its answer.
    private boolean setupQuestionShown = false;

    private void startIntro() {
        if (intro == null) {
            intro = IntroPlayer.startIfWanted(this, preferences, false);
        }
    }

    boolean introActive() {
        return intro != null && intro.isActive();
    }

    // The opening goes under the touch controls' layer (the controls themselves are hidden while it is on).
    int introLayerIndex() {
        ViewGroup layout = (ViewGroup) getContentView();
        int i = overlayView != null ? layout.indexOfChild(overlayView) : -1;
        return i >= 0 ? i : layout.getChildCount();
    }

    // Called from the game (IntroVideo.cpp) when it is reset: the opening plays again, from the logo.
    void ReplayIntro() {
        runOnUiThread(() -> {
            if (introActive()) {
                return;
            }
            intro = IntroPlayer.startIfWanted(this, preferences, true);
        });
    }

    // The opening video while it is on screen (IntroPlayer.java), null otherwise.
    private IntroPlayer intro;

    // Mutes the game's sound while the opening video plays (BenPort.cpp); it fades back in after.
    static native void nativeSetIntroPlaying(boolean playing);
    static native void nativeSetIntroSound(boolean on);

    // The opening stops with the app and goes on where it was when the app comes back.
    @Override
    protected void onPause() {
        if (intro != null && intro.isActive()) {
            intro.pause();
        }
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Back from the app's settings, where the storage permission may have been allowed.
        recheckStorageAccess(false);
        if (intro != null && intro.isActive() && !introPausedForSetup) {
            intro.resume();
        }
    }

    @Override
    protected void onDestroy() {
        // The game's thread may still wait for the files; it must be released, or SDL's onDestroy waits for it
        // forever.
        setupGate.cancel("app fechado");
        super.onDestroy();
    }

    // <data folder>/inicio.log: each launch starts it, the native side (mm/2s2h/Android/StartupLog.cpp) adds to it.
    // Without it, a crash before the game's own log opens leaves nothing to look at.
    private static final String STARTUP_LOG = "inicio.log";

    private void startStartupLog() {
        File log = new File(new File(Environment.getExternalStorageDirectory(), DATA_FOLDER), STARTUP_LOG);
        log.delete();
        startupLog("App aberto: versão " + BuildConfig.VERSION_NAME + " (" + BuildConfig.VERSION_CODE + "), Android "
                + Build.VERSION.RELEASE + ", " + Build.MANUFACTURER + " " + Build.MODEL);
        Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            StringWriter trace = new StringWriter();
            error.printStackTrace(new PrintWriter(trace));
            startupLog("FECHOU (Java, " + thread.getName() + "): " + trace);
            if (previous != null) {
                previous.uncaughtException(thread, error);
            }
        });
    }

    static synchronized void startupLog(String line) {
        Log.i("2ShipStartup", line);
        File folder = new File(Environment.getExternalStorageDirectory(), DATA_FOLDER);
        try (FileWriter out = new FileWriter(new File(folder, STARTUP_LOG), true)) {
            String time = new SimpleDateFormat("HH:mm:ss", Locale.US).format(new Date());
            out.write("[" + time + "] " + line + "\n");
        } catch (IOException e) {
            // The folder may not exist or be writable yet (no storage permission on the first launch).
        }
    }

    // Progress while the game makes mm.o2r (Extract.cpp calls this a few times a second), on the preparation screen.
    // total 0: starting; done == total: writing the file; -1: finished.
    public static void showExtractionProgress(final int done, final int total) {
        final MainActivity activity = (MainActivity) mSingleton;
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(() -> {
            String title = "Preparando o jogo";
            String text = "Criando o mm.o2r com os arquivos da sua ROM. Isso só acontece uma vez e pode levar alguns "
                    + "minutos.";
            if (done < 0) {
                SetupOverlay.progress(activity, title, "Abrindo os arquivos do jogo...", -1f);
                startupLog("Extração: barra fechada");
            } else if (total <= 0) {
                SetupOverlay.progress(activity, title, text, -1f);
            } else if (done >= total) {
                SetupOverlay.progress(activity, title, text + "\n\nGravando o mm.o2r...", 1f);
            } else {
                SetupOverlay.progress(activity, title, text + "\n\n" + done + " de " + total + " arquivos",
                        (float) done / total);
            }
        });
    }

    // ----- Startup: storage access, then the port's files, then the game
    //
    // The game's thread (mm/src/code/main.c) waits in waitForSetupFromNative() and only starts once setupGate is
    // opened, which happens only when the port's files in the data folder are complete and checked (PortInstall).
    // The wait always ends: the gate is cancelled when the person chooses to leave, when the activity is destroyed,
    // or after GATE_TIMEOUT_MS. Every refusal of a permission or failed copy shows a message with a way to try again.

    private static final SetupGate setupGate = new SetupGate();
    // Nothing the app does before the game starts takes this long; past it the wait is given up (the app closes).
    private static final long GATE_TIMEOUT_MS = 30L * 60L * 1000L;
    // A copy of the port's files takes about 30 seconds; past this the person is asked whether to keep waiting.
    private static final long INSTALL_SLOW_MS = 5L * 60L * 1000L;
    // Free space wanted before the port's files are copied (assets and 2ship.o2r, with room to spare).
    private static final long INSTALL_FREE_BYTES = 256L << 20;

    // The storage permission was asked for and the answer has not come back yet.
    private boolean waitingForStorageAccess = false;
    // onStorageReady() ran: the rest of the startup is under way.
    private boolean storageFlowDone = false;
    private volatile boolean installRunning = false;

    // From the game's thread: true when the files are ready and the game may start, false when it must not (the
    // person left, or the time ran out); main.c then returns without starting the game.
    public static boolean waitForSetupFromNative() {
        try {
            boolean ready = setupGate.await(GATE_TIMEOUT_MS, 30000,
                    waited -> startupLog("Jogo ainda esperando a preparação dos arquivos (" + (waited / 1000) + " s)"));
            if (!ready) {
                startupLog("O jogo não vai abrir: " + setupGate.reason());
                final MainActivity activity = (MainActivity) mSingleton;
                if (activity != null && setupGate.reason().startsWith("tempo")) {
                    activity.runOnUiThread(() -> Toast.makeText(activity,
                            "A preparação dos arquivos não terminou a tempo. Abra o app de novo.", Toast.LENGTH_LONG)
                            .show());
                }
            }
            return ready;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            setupGate.cancel("interrompido");
            return false;
        }
    }

    private static File dataFolder() {
        return new File(Environment.getExternalStorageDirectory(), DATA_FOLDER);
    }

    // Android 11+: "all files access" (MANAGE_EXTERNAL_STORAGE). Android 7-10: READ and WRITE_EXTERNAL_STORAGE
    // (with requestLegacyExternalStorage in the manifest for Android 10).
    private boolean hasStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void beginStartup() {
        if (hasStorageAccess()) {
            onStorageReady();
        } else {
            requestStorageAccess();
        }
    }

    private void requestStorageAccess() {
        waitingForStorageAccess = true;
        startupLog("Pedindo a permissão de arquivos");
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
            intent.setData(Uri.parse("package:" + getPackageName()));
            try {
                startActivityForResult(intent, STORAGE_PERMISSION_REQUEST_CODE);
            } catch (android.content.ActivityNotFoundException e) {
                // Some systems only have the general list of apps with that access.
                try {
                    startActivityForResult(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION),
                            STORAGE_PERMISSION_REQUEST_CODE);
                } catch (android.content.ActivityNotFoundException e2) {
                    showStorageDenied(true);
                }
            }
        } else {
            requestPermissions(new String[] { Manifest.permission.READ_EXTERNAL_STORAGE,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE }, STORAGE_PERMISSION_REQUEST_CODE);
        }
    }

    // Android 7-10: the answer to requestPermissions().
    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != STORAGE_PERMISSION_REQUEST_CODE) {
            return;
        }
        if (hasStorageAccess()) {
            startupLog("Permissão de arquivos concedida");
            waitingForStorageAccess = false;
            onStorageReady();
            return;
        }
        // An empty answer means the request was interrupted (the activity went away): ask again on return.
        if (grantResults == null || grantResults.length == 0) {
            startupLog("Pedido de permissão interrompido");
            return;
        }
        // No rationale after a refusal means "don't ask again" was chosen: only the app's settings can change it.
        boolean canAskAgain = shouldShowRequestPermissionRationale(Manifest.permission.READ_EXTERNAL_STORAGE)
                || shouldShowRequestPermissionRationale(Manifest.permission.WRITE_EXTERNAL_STORAGE);
        startupLog("Permissão de arquivos negada" + (canAskAgain ? "" : " (não perguntar de novo)"));
        showStorageDenied(!canAskAgain);
    }

    // Explains the refusal and offers a new try (or the app's settings, where the system no longer asks), or to
    // leave. Nothing is lost: the app can be opened again later and asks again.
    private void showStorageDenied(boolean viaSettings) {
        String text = "Sem acesso aos arquivos, o jogo não consegue ler a ROM, guardar os saves nem usar mods e "
                + "traduções na pasta " + DATA_FOLDER + ".\n\n";
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            text += "Na tela seguinte, ative \"Permitir acesso para gerenciar todos os arquivos\" para este app.";
        } else if (viaSettings) {
            text += "O Android não vai perguntar de novo. Abra as configurações do app, toque em Permissões e "
                    + "permita Arquivos e mídia (ou Armazenamento).";
        } else {
            text += "Toque em Tentar de novo e permita o acesso.";
        }
        String retry = viaSettings && Build.VERSION.SDK_INT < Build.VERSION_CODES.R ? "Abrir configurações"
                : "Tentar de novo";
        SetupOverlay.ask(this, "Permissão necessária", text, new String[] { retry, "Sair" }, 0, 1, index -> {
            if (index == 0) {
                if (viaSettings && Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
                    waitingForStorageAccess = true;
                    openAppSettings();
                } else {
                    requestStorageAccess();
                }
            } else {
                leaveBeforeGame("o acesso aos arquivos foi negado");
            }
        });
    }

    private void openAppSettings() {
        Intent intent = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
        intent.setData(Uri.parse("package:" + getPackageName()));
        try {
            startActivity(intent);
        } catch (android.content.ActivityNotFoundException e) {
            Toast.makeText(this, "Abra Configurações > Apps > " + getString(R.string.app_name) + " > Permissões.",
                    Toast.LENGTH_LONG).show();
        }
    }

    // Back from the system's permission screens: onActivityResult (fromResult) or onResume, which also covers the
    // app's settings and a permission allowed by hand while the refusal message was up. Only the result of the
    // "all files access" screen counts as a refusal; onResume also runs while that screen is opening.
    private void recheckStorageAccess(boolean fromResult) {
        if (storageFlowDone) {
            return;
        }
        if (hasStorageAccess()) {
            startupLog("Permissão de arquivos concedida");
            waitingForStorageAccess = false;
            SetupOverlay.hide(this);
            onStorageReady();
        } else if (fromResult && waitingForStorageAccess && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            waitingForStorageAccess = false;
            startupLog("Acesso a todos os arquivos não concedido");
            showStorageDenied(true);
        }
    }

    // The person chose not to go on: the game's thread is released without starting the game, and the app closes.
    private void leaveBeforeGame(String why) {
        startupLog("Saindo antes do jogo: " + why);
        setupGate.cancel(why);
        SetupOverlay.hide(this);
        finish();
    }

    private static final int STORAGE_PERMISSION_REQUEST_CODE = 2296;
    private static final int FILE_PICKER_REQUEST_CODE = 0;
    private static final int TRANSLATION_PICKER_REQUEST_CODE = 3;
    private static final int MOD_PICKER_REQUEST_CODE = 4;
    private static final int MUSIC_PICKER_REQUEST_CODE = 5;
    // Must match mm/2s2h/Enhancements/Audio/MusicFolder.cpp (the library is "musicas" in the data folder).
    private static final String MUSIC_IMPORT_FOLDER = "musicas/.origem";
    // Must match mm/2s2h/Mods/ModInstaller.cpp.
    private static final String MODS_FOLDER = "mods";
    // Must match mm/2s2h/Language/GameLanguage.cpp.
    private static final String TRANSLATION_FOLDER = "traducoes/pt-BR";
    private static final String LANGUAGE_FILE = "idioma.txt";

    // Moves mm.o2r, saves, mods and the rest from the folder used before the rename. Files already in the new folder
    // are kept; whatever could not be moved stays in the old folder.
    private void migrateOldDataFolder() {
        File storage = Environment.getExternalStorageDirectory();
        File oldFolder = new File(storage, OLD_DATA_FOLDER);
        File newFolder = new File(storage, DATA_FOLDER);
        File[] children = oldFolder.listFiles();
        if (children == null) {
            return;
        }
        if (!newFolder.isDirectory() && oldFolder.renameTo(newFolder)) {
            startupLog("Pasta " + OLD_DATA_FOLDER + " renomeada para " + DATA_FOLDER);
            return;
        }
        newFolder.mkdirs();
        int moved = 0;
        for (File child : children) {
            File target = new File(newFolder, child.getName());
            if (!target.exists() && child.renameTo(target)) {
                moved++;
            }
        }
        oldFolder.delete(); // only succeeds when everything was moved
        startupLog(moved + " itens movidos de " + OLD_DATA_FOLDER + " para " + DATA_FOLDER);
    }

    // Saves of the very first builds, kept in the app's own folder (Android/data/<package>/files).
    private void migrateAndroidDataLegacy(File targetRootFolder) {
        File sourceOldRoot = getExternalFilesDir(null);
        File sourceSavesDir = new File(sourceOldRoot, "saves"); // how to tell if there's anything to migrate

        // === Migration from old Android/data/.../files/ directory ===
        if (sourceOldRoot != null && sourceSavesDir.isDirectory()) {
            Log.i("setupFiles", "Migrating old data from: " + sourceOldRoot.getAbsolutePath());

            File[] sourceFiles = sourceOldRoot.listFiles();
            if (sourceFiles != null) {
                for (File file : sourceFiles) {
                    String name = file.getName();
                    if (name.equals("assets") || name.equals("2ship.o2r") || name.equals("mm.o2r")) {
                        continue; // Skip these
                    }

                    File dest = new File(targetRootFolder, name);
                    try {
                        if (file.isDirectory()) {
                            AssetCopyUtil.copyDirectory(file, dest);
                        } else {
                            AssetCopyUtil.copyFile(file, dest);
                        }
                        Log.i("setupFiles", "Migrated: " + name);
                    } catch (IOException e) {
                        Log.e("setupFiles", "Failed to migrate: " + name, e);
                    }
                }
            }

            runOnUiThread(() -> Toast.makeText(this, "Saves migrados", Toast.LENGTH_SHORT).show());
        }

    }

    private String portFilesStamp() {
        long installedAt = 0;
        try {
            installedAt = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
        } catch (PackageManager.NameNotFoundException e) {
            Log.w("setupFiles", "No package info", e);
        }
        return BuildConfig.VERSION_CODE + "/" + installedAt;
    }

    private PortInstall portInstall() {
        return new PortInstall(dataFolder(), MainActivity::startupLog);
    }

    private PortInstall.Source apkAssets() {
        final android.content.res.AssetManager assets = getAssets();
        return new PortInstall.Source() {
            @Override
            public String[] list(String path) throws IOException {
                return assets.list(path);
            }

            @Override
            public InputStream open(String path) throws IOException {
                return assets.open(path);
            }
        };
    }

    // Storage access is there: older installs are brought in, an install stopped half way is put back, and the
    // port's files are checked against this APK. The game starts at once when they match.
    private void onStorageReady() {
        if (storageFlowDone) {
            return;
        }
        storageFlowDone = true;
        File root = dataFolder();
        migrateOldDataFolder();
        startupLog("Conferindo os arquivos");
        try {
            FileOps.mkdirs(root);
        } catch (IOException e) {
            startupLog("Erro: " + e.getMessage());
            showSetupError("Não foi possível criar a pasta " + DATA_FOLDER + " no armazenamento do aparelho.", true);
            return;
        }
        PortInstall install = portInstall();
        install.recover();
        dropGameArchiveBackupIfReplaced(root);

        String installed = install.installedStamp();
        String wanted = portFilesStamp();
        boolean needsInstall = !wanted.equals(installed);
        boolean needsMigration = false;
        startupLog("Arquivos do app: " + (installed == null ? "incompletos ou ausentes" : "build " + installed)
                + (needsInstall ? ", precisa instalar o build " + wanted : ", em dia")
                + (needsMigration ? "; dados antigos em Android/data para migrar" : ""));

        if (needsInstall) {
            setupQuestionShown = true;
            SetupOverlay.ask(this, "Bem-vindo",
                    "Antes de jogar, o app vai copiar os arquivos dele para a pasta " + DATA_FOLDER
                            + ". Leva cerca de 30 segundos.",
                    new String[] { "Começar" }, 0, -1, index -> {
                        setupQuestionShown = false;
                        runSetupInBackground(true);
                    });
        } else if (needsMigration) {
            runSetupInBackground(false);
        } else {
            setupGate.open();
            startIntroOrWaitForSetup();
        }
    }

    // Migration of Android/data, then (when `install`) the port's files, on a background thread. The game's
    // thread is released only when everything it needs is there.
    private void runSetupInBackground(boolean install) {
        SetupOverlay.progress(this, "Configuração inicial", install
                ? "Copiando os arquivos do app para a pasta " + DATA_FOLDER + "..."
                : "Trazendo os seus arquivos da instalação antiga...", -1f);
        installRunning = true;
        new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(this::checkSlowInstall,
                INSTALL_SLOW_MS);
        Executors.newSingleThreadExecutor().execute(() -> {
            File root = dataFolder();
            if (install) {
                migrateAndroidDataLegacy(root);
            }
            String error = null;
            if (install) {
                try {
                    FileOps.mkdirs(root);
                } catch (IOException e) {
                    error = "Não foi possível criar a pasta " + DATA_FOLDER + " no armazenamento do aparelho.";
                }
                if (error != null) {
                    // shown below
                } else if (root.getUsableSpace() < INSTALL_FREE_BYTES) {
                    error = "Não há espaço livre suficiente no aparelho (são precisos uns "
                            + (INSTALL_FREE_BYTES >> 20) + " MB). Libere espaço e tente de novo.";
                } else {
                    try {
                        portInstall().install(apkAssets(), portFilesStamp());
                        startupLog("Arquivos do app instalados e conferidos");
                        handleGameArchiveVersion(root);
                    } catch (IOException e) {
                        startupLog("Erro ao instalar os arquivos do app: " + e.getMessage());
                        error = "Não foi possível copiar os arquivos do app: " + e.getMessage()
                                + "\n\nOs arquivos que já estavam na pasta não foram alterados.";
                    }
                }
            }
            installRunning = false;
            final String failure = error;
            runOnUiThread(() -> {
                if (isFinishing()) {
                    return;
                }
                if (failure != null) {
                    showSetupError(failure, true);
                    return;
                }
                setupGate.open();
                // The opening plays now, or after the game prepared its files on the same screen.
                if (gameFilesMissing()) {
                    introWaitsForSetup = true;
                    SetupOverlay.progress(this, "Preparando o jogo", "Um momento...", -1f);
                } else {
                    SetupOverlay.hide(this);
                    startIntro();
                }
            });
        });
    }

    private void checkSlowInstall() {
        if (!installRunning || isFinishing()) {
            return;
        }
        startupLog("A cópia dos arquivos passou de " + (INSTALL_SLOW_MS / 60000) + " minutos");
        SetupOverlay.ask(this, "Demorando mais que o normal",
                "A cópia dos arquivos do app ainda não terminou. Pode ser um cartão ou armazenamento lento.",
                new String[] { "Continuar esperando", "Sair" }, 0, 0, index -> {
                    if (index == 1) {
                        leaveBeforeGame("a cópia demorou e a pessoa saiu");
                        return;
                    }
                    if (installRunning) {
                        SetupOverlay.progress(this, "Configuração inicial", "Copiando os arquivos do app...", -1f);
                        new android.os.Handler(android.os.Looper.getMainLooper())
                                .postDelayed(this::checkSlowInstall, INSTALL_SLOW_MS);
                    }
                });
    }

    // A failed install: the working files (if any) are untouched. Offers a new try, playing with the files already
    // there when they are complete (from an earlier build), or leaving.
    private void showSetupError(String text, boolean canRetry) {
        boolean canKeep = portInstall().installedStamp() != null;
        java.util.List<String> buttons = new java.util.ArrayList<>();
        if (canRetry) {
            buttons.add("Tentar de novo");
        }
        if (canKeep) {
            buttons.add("Jogar com os arquivos atuais");
        }
        buttons.add("Sair");
        final String[] labels = buttons.toArray(new String[0]);
        SetupOverlay.ask(this, "Não foi possível preparar o jogo", text, labels, 0, labels.length - 1, index -> {
            String choice = labels[Math.max(0, Math.min(index, labels.length - 1))];
            if (choice.equals("Tentar de novo")) {
                runSetupInBackground(true);
            } else if (choice.equals("Jogar com os arquivos atuais")) {
                startupLog("Seguindo com os arquivos do build " + portInstall().installedStamp());
                SetupOverlay.hide(this);
                setupGate.open();
                startIntroOrWaitForSetup();
            } else {
                leaveBeforeGame("a preparação falhou");
            }
        });
    }

    // ----- The game's own archive (mm.o2r, made from the ROM)
    //
    // A new versionCode means a new port version whose mm.o2r must be made again. The old one is kept aside in
    // .mm-o2r-anterior (not deleted) until the new one exists, and the version is only recorded once that is done.
    // The version is also written in the data folder, so a reinstall (which clears the app's preferences) does not
    // throw away a mm.o2r that is already right.

    private static final String GAME_ARCHIVE = "mm.o2r";
    private static final String GAME_ARCHIVE_BACKUP = ".mm-o2r-anterior";
    private static final String GAME_ARCHIVE_VERSION = "mm.o2r.versao";

    private int storedPortVersion(File root) {
        if (preferences.contains("appVersion")) {
            return preferences.getInt("appVersion", 1);
        }
        try {
            return Integer.parseInt(new String(FileOps.readAll(new File(root, GAME_ARCHIVE_VERSION)),
                    java.nio.charset.StandardCharsets.UTF_8).trim());
        } catch (IOException | NumberFormatException e) {
            return 1;
        }
    }

    private void handleGameArchiveVersion(File root) throws IOException {
        int current = BuildConfig.VERSION_CODE;
        int stored = storedPortVersion(root);
        File archive = new File(root, GAME_ARCHIVE);
        if (current > stored && archive.isFile()) {
            File backup = new File(root, GAME_ARCHIVE_BACKUP);
            FileOps.mkdirs(backup);
            FileOps.move(archive, new File(backup, GAME_ARCHIVE));
            startupLog("Versão nova (" + stored + " -> " + current + "): mm.o2r antigo guardado em "
                    + GAME_ARCHIVE_BACKUP + " até o novo ser criado");
        }
        FileOps.writeAtomically(new File(root, GAME_ARCHIVE_VERSION),
                (current + "\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
        preferences.edit().putInt("appVersion", current).apply();
    }

    private void dropGameArchiveBackupIfReplaced(File root) {
        File backup = new File(root, GAME_ARCHIVE_BACKUP);
        File archive = new File(root, GAME_ARCHIVE);
        if (backup.exists() && archive.isFile() && archive.length() > 0) {
            startupLog("mm.o2r novo criado: apagando o antigo guardado");
            FileOps.deleteRecursive(backup);
        }
    }

    private native void nativeHandleSelectedFile(String filePath);

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        if (requestCode == TRANSLATION_PICKER_REQUEST_CODE) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                installTranslationPack(data.getData());
            }
        } else if (requestCode == MUSIC_PICKER_REQUEST_CODE) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                final Uri tree = data.getData();
                Executors.newSingleThreadExecutor().execute(() -> copyMusicFolder(tree));
            }
        } else if (requestCode == MOD_PICKER_REQUEST_CODE) {
            if (resultCode == RESULT_OK && data != null) {
                if (data.getClipData() != null) {
                    for (int i = 0; i < data.getClipData().getItemCount(); i++) {
                        installMod(data.getClipData().getItemAt(i).getUri());
                    }
                } else if (data.getData() != null) {
                    installMod(data.getData());
                }
                nativeModsInstalled();
            }
        } else if (requestCode == FILE_PICKER_REQUEST_CODE && (resultCode != RESULT_OK || data == null)) {
            // Picker closed without a file: unblock the extractor, which reports that no ROM was chosen.
            nativeHandleSelectedFile("");
        } else if (requestCode == FILE_PICKER_REQUEST_CODE) {
            // Handle file selection
            Uri selectedFileUri = data.getData();
            String fileName = "MM.z64";

            File destinationDirectory = new File(Environment.getExternalStorageDirectory(), DATA_FOLDER);
            File destinationFile = new File(destinationDirectory, fileName);

            if (destinationDirectory != null && selectedFileUri != null) {
                try {
                    InputStream in = getContentResolver().openInputStream(selectedFileUri);
                    OutputStream out = new FileOutputStream(destinationFile);

                    byte[] buffer = new byte[4096];
                    int bytesRead;
                    while ((bytesRead = in.read(buffer)) != -1) {
                        out.write(buffer, 0, bytesRead);
                    }

                    in.close();
                    out.close();
                } catch (IOException e) {
                    e.printStackTrace();
                    Toast.makeText(this, "Erro ao copiar a ROM: " + e.getMessage(), Toast.LENGTH_LONG).show();
                    nativeHandleSelectedFile("");
                    return;
                }
            }

            // Now pass the path of the file in the new folder
            nativeHandleSelectedFile(destinationFile.getPath());

        } else if (requestCode == STORAGE_PERMISSION_REQUEST_CODE) {
            // Back from the "all files access" screen (Android 11+).
            recheckStorageAccess(true);
        }
    }


    // Called from the menu (Configurações > Idioma): pick a translation pack and install it as the only pack
    // for Portuguese, then select Portuguese for the next launch.
    public void openTranslationPicker() {
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            startActivityForResult(intent, TRANSLATION_PICKER_REQUEST_CODE);
        });
    }

    // Called from the Mods menu: pick one or more mods and copy them into the mods folder.
    public void openMusicFolderPicker() {
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            startActivityForResult(intent, MUSIC_PICKER_REQUEST_CODE);
        });
    }

    private static boolean isMusicFileName(String name) {
        String lower = name.toLowerCase();
        return lower.endsWith(".wav") || lower.endsWith(".flac") || lower.endsWith(".mp3") || lower.endsWith(".ogg")
                || lower.endsWith(".opus") || lower.endsWith(".oga") || lower.equals("loops.txt");
    }

    // Copies the songs (and loops.txt) of the picked folder into the data folder; the game converts them from
    // there into its music library and deletes the copy.
    private void copyMusicFolder(Uri tree) {
        File target = new File(new File(Environment.getExternalStorageDirectory(), DATA_FOLDER), MUSIC_IMPORT_FOLDER);
        deleteRecursively(target);
        target.mkdirs();
        runOnUiThread(() -> Toast.makeText(this, "Copiando as músicas...", Toast.LENGTH_SHORT).show());

        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree,
                DocumentsContract.getTreeDocumentId(tree));
        String[] columns = { DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE };
        int copied = 0;
        try (Cursor cursor = getContentResolver().query(children, columns, null, null, null)) {
            while (cursor != null && cursor.moveToNext()) {
                String id = cursor.getString(0);
                String name = cursor.getString(1);
                if (name == null || DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2))
                        || !isMusicFileName(name)) {
                    continue;
                }
                Uri file = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                try (InputStream in = getContentResolver().openInputStream(file);
                     OutputStream out = new FileOutputStream(new File(target, name))) {
                    copyStream(in, out);
                    copied++;
                } catch (IOException e) {
                    Log.e("music", "Failed to copy " + name, e);
                }
            }
        } catch (Exception e) {
            Log.e("music", "Failed to list the music folder", e);
        }
        final int count = copied;
        runOnUiThread(() -> Toast.makeText(this, count + " arquivo(s) copiado(s). Convertendo no menu Músicas...",
                Toast.LENGTH_LONG).show());
        nativeMusicFolderCopied(target.getAbsolutePath());
    }

    private static void deleteRecursively(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteRecursively(child);
            }
        }
        file.delete();
    }

    public native void nativeMusicFolderCopied(String path);

    public native void nativeModsInstalled();
    public native void nativeDriveLinkEntered(String link);
    public native void nativeDownloadProgress(long done, long total);

    // Settings > Mods > Baixar do Google Drive: a box for the link, filled with the clipboard when it holds one.
    public void askDriveLink() {
        runOnUiThread(() -> {
            final android.widget.EditText input = new android.widget.EditText(this);
            input.setSingleLine(true);
            input.setHint("https://drive.google.com/...");
            android.content.ClipboardManager clipboard =
                (android.content.ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
            if (clipboard != null && clipboard.hasPrimaryClip() && clipboard.getPrimaryClip().getItemCount() > 0) {
                CharSequence clip = clipboard.getPrimaryClip().getItemAt(0).coerceToText(this);
                if (clip != null && clip.toString().contains("drive.google.com")) {
                    input.setText(clip.toString().trim());
                }
            }
            int pad = (int) (20 * getResources().getDisplayMetrics().density);
            FrameLayout box = new FrameLayout(this);
            box.setPadding(pad, pad / 2, pad, 0);
            box.addView(input);
            new AlertDialog.Builder(this)
                .setTitle("Baixar mod do Google Drive")
                .setMessage("Cole o link de um arquivo de mod (.o2r, .otr, .zip) ou de uma pasta com mods. "
                            + "No Drive, o compartilhamento deve estar em \"Qualquer pessoa com o link\".")
                .setView(box)
                .setPositiveButton("Baixar", (dialog, which) -> nativeDriveLinkEntered(input.getText().toString()))
                .setNegativeButton("Cancelar", null)
                .show();
        });
    }

    // Called by the game on its own download thread: GET `url` into `path`, following redirects. Returns
    // "ok\n<file name>\n<content type>" or "erro\n<message>".
    public String httpDownload(String url, String path) {
        java.net.HttpURLConnection connection = null;
        try {
            java.net.URL target = new java.net.URL(url);
            for (int hops = 0; hops < 8; hops++) {
                connection = (java.net.HttpURLConnection) target.openConnection();
                connection.setInstanceFollowRedirects(false);
                connection.setConnectTimeout(20000);
                connection.setReadTimeout(60000);
                connection.setRequestProperty("User-Agent", "MajorasMaskDefinitive-Mods/1.0");
                int code = connection.getResponseCode();
                if (code >= 300 && code < 400 && connection.getHeaderField("Location") != null) {
                    target = new java.net.URL(target, connection.getHeaderField("Location"));
                    connection.disconnect();
                    connection = null;
                    continue;
                }
                if (code != 200) {
                    return "erro\no Google Drive respondeu " + code;
                }
                break;
            }
            if (connection == null) {
                return "erro\nredirecionamentos demais";
            }
            String disposition = connection.getHeaderField("Content-Disposition");
            String type = connection.getContentType();
            long total = connection.getContentLengthLong();
            String name = "";
            if (disposition != null) {
                java.util.regex.Matcher star =
                    java.util.regex.Pattern.compile("filename\\*\\s*=\\s*[^']*'[^']*'([^;]+)", java.util.regex.Pattern.CASE_INSENSITIVE)
                        .matcher(disposition);
                java.util.regex.Matcher quoted =
                    java.util.regex.Pattern.compile("filename\\s*=\\s*\"([^\"]+)\"", java.util.regex.Pattern.CASE_INSENSITIVE)
                        .matcher(disposition);
                if (star.find()) {
                    name = java.net.URLDecoder.decode(star.group(1).trim().replace("+", "%2B"), "UTF-8");
                } else if (quoted.find()) {
                    name = quoted.group(1);
                }
            }
            File file = new File(path);
            File parent = file.getParentFile();
            if (parent != null && !parent.isDirectory()) {
                parent.mkdirs();
            }
            try (InputStream in = connection.getInputStream(); OutputStream out = new FileOutputStream(file)) {
                byte[] buffer = new byte[65536];
                long done = 0;
                long lastReport = 0;
                int read;
                while ((read = in.read(buffer)) != -1) {
                    out.write(buffer, 0, read);
                    done += read;
                    if (done - lastReport > 262144) {
                        nativeDownloadProgress(done, total);
                        lastReport = done;
                    }
                }
                nativeDownloadProgress(done, total);
            }
            return "ok\n" + name.replace("\n", " ") + "\n" + (type == null ? "" : type);
        } catch (Exception e) {
            Log.e("mods", "Download failed", e);
            String message = e.getMessage();
            if (e instanceof java.net.UnknownHostException) {
                message = "sem internet";
            }
            return "erro\n" + (message == null ? e.getClass().getSimpleName() : message.replace("\n", " "));
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    public void openModPicker() {
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            startActivityForResult(intent, MOD_PICKER_REQUEST_CODE);
        });
    }

    private static boolean isModArchiveName(String name) {
        String lower = name.toLowerCase();
        return lower.endsWith(".o2r") || lower.endsWith(".otr") || lower.endsWith(".zip");
    }

    private static void copyStream(InputStream in, OutputStream out) throws IOException {
        byte[] buffer = new byte[65536];
        int read;
        while ((read = in.read(buffer)) != -1) {
            out.write(buffer, 0, read);
        }
    }

    // Copies a mod into the mods folder. A .zip that only wraps .o2r/.otr files (how mods are often downloaded)
    // has those files taken out; any other .zip is a mod archive itself and is copied as it is.
    private void installMod(Uri uri) {
        File folder = new File(new File(Environment.getExternalStorageDirectory(), DATA_FOLDER), MODS_FOLDER);

        String name = null;
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (column >= 0) {
                    name = cursor.getString(column);
                }
            }
        } catch (Exception e) {
            Log.w("mods", "Could not read the file name", e);
        }
        if (name == null || name.isEmpty()) {
            name = "mod.o2r";
        }
        name = name.replaceAll("[/\\\\]", "_");
        if (!isModArchiveName(name)) {
            Toast.makeText(this, "Escolha um mod .o2r, .otr ou .zip (" + name + ")", Toast.LENGTH_LONG).show();
            return;
        }
        if (!folder.isDirectory() && !folder.mkdirs()) {
            Toast.makeText(this, "Não foi possível criar a pasta " + folder.getPath(), Toast.LENGTH_LONG).show();
            return;
        }

        java.util.List<String> installed = new java.util.ArrayList<>();
        try {
            if (name.toLowerCase().endsWith(".zip")) {
                // First pass: does the zip only wrap mod archives?
                boolean hasArchives = false;
                boolean hasOther = false;
                try (java.util.zip.ZipInputStream zip =
                         new java.util.zip.ZipInputStream(getContentResolver().openInputStream(uri))) {
                    java.util.zip.ZipEntry entry;
                    while ((entry = zip.getNextEntry()) != null) {
                        if (entry.isDirectory()) {
                            continue;
                        }
                        String entryName = new File(entry.getName()).getName();
                        String lower = entryName.toLowerCase();
                        if (lower.endsWith(".o2r") || lower.endsWith(".otr")) {
                            hasArchives = true;
                        } else if (!lower.endsWith(".txt") && !lower.endsWith(".md") && !lower.endsWith(".pdf") &&
                                   !lower.endsWith(".png") && !lower.endsWith(".jpg")) {
                            hasOther = true;
                        }
                    }
                }
                if (hasArchives && !hasOther) {
                    try (java.util.zip.ZipInputStream zip =
                             new java.util.zip.ZipInputStream(getContentResolver().openInputStream(uri))) {
                        java.util.zip.ZipEntry entry;
                        while ((entry = zip.getNextEntry()) != null) {
                            String entryName = new File(entry.getName()).getName();
                            String lower = entryName.toLowerCase();
                            if (entry.isDirectory() || !(lower.endsWith(".o2r") || lower.endsWith(".otr"))) {
                                continue;
                            }
                            try (OutputStream out = new FileOutputStream(new File(folder, entryName))) {
                                copyStream(zip, out);
                            }
                            installed.add(entryName);
                        }
                    }
                }
            }
            if (installed.isEmpty()) {
                try (InputStream in = getContentResolver().openInputStream(uri);
                     OutputStream out = new FileOutputStream(new File(folder, name))) {
                    copyStream(in, out);
                }
                installed.add(name);
            }
        } catch (Exception e) {
            Log.e("mods", "Failed to install the mod", e);
            Toast.makeText(this, "Erro ao instalar o mod: " + e.getMessage(), Toast.LENGTH_LONG).show();
            return;
        }

        Toast.makeText(this, "Mod instalado: " + android.text.TextUtils.join(", ", installed) + ".",
                       Toast.LENGTH_LONG).show();
    }

    private void installTranslationPack(Uri uri) {
        File root = new File(Environment.getExternalStorageDirectory(), DATA_FOLDER);
        File folder = new File(root, TRANSLATION_FOLDER);

        String name = null;
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (column >= 0) {
                    name = cursor.getString(column);
                }
            }
        } catch (Exception e) {
            Log.w("translation", "Could not read the file name", e);
        }
        if (name == null || name.isEmpty()) {
            name = "traducao.o2r";
        }
        name = name.replaceAll("[/\\\\]", "_");
        String lower = name.toLowerCase();
        if (!lower.endsWith(".o2r") && !lower.endsWith(".zip") && !lower.endsWith(".otr")) {
            Toast.makeText(this, "Escolha um arquivo .o2r, .zip ou .otr", Toast.LENGTH_LONG).show();
            return;
        }

        if (!folder.isDirectory() && !folder.mkdirs()) {
            Toast.makeText(this, "Não foi possível criar a pasta " + folder.getPath(), Toast.LENGTH_LONG).show();
            return;
        }
        File[] oldPacks = folder.listFiles();
        if (oldPacks != null) {
            for (File old : oldPacks) {
                if (old.isFile()) {
                    old.delete();
                }
            }
        }

        File target = new File(folder, name);
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[65536];
            int read;
            while ((read = in.read(buffer)) != -1) {
                out.write(buffer, 0, read);
            }
        } catch (Exception e) {
            Log.e("translation", "Failed to copy the translation pack", e);
            target.delete();
            Toast.makeText(this, "Erro ao copiar a tradução: " + e.getMessage(), Toast.LENGTH_LONG).show();
            return;
        }

        try (OutputStream out = new FileOutputStream(new File(root, LANGUAGE_FILE))) {
            out.write("pt-BR\n".getBytes());
        } catch (IOException e) {
            Log.e("translation", "Failed to save the language", e);
        }

        Toast.makeText(this, "Tradução instalada: " + name + ". Feche e abra o app para usar.", Toast.LENGTH_LONG)
                .show();
    }

    public void openFilePicker() {
        // Create an Intent to open the file picker dialog
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.setType("*/*");

        // Start the file picker dialog
        startActivityForResult(intent, 0);
    }

    // Check if external storage is available and writable
    private boolean isExternalStorageWritable() {
        String state = Environment.getExternalStorageState();
        return Environment.MEDIA_MOUNTED.equals(state);
    }


    public native void attachController();
    public native void detachController();
    // Native method for setting button state
    public native void setButton(int button, boolean value);
    public native void setCameraState(int axis, float value);

    // Native method for setting joystick axis value
    public native void setAxis(int axis, short value);

    private Button button1, button2, button3, button4;
    private Button buttonA, buttonB, buttonX, buttonY;
    private Button buttonDpadUp, buttonDpadDown, buttonDpadLeft, buttonDpadRight;
    private Button buttonLB, buttonRB, buttonZ, buttonStart, buttonBack;
    private Button buttonToggle;
    private FrameLayout leftJoystick;
    private ImageView leftJoystickKnob;
    private View overlayView;

    // Function to set up the controller overlay (inflate layout and initialize buttons)
    private void setupControllerOverlay() {
        // Inflate the touchcontrol_overlay layout
        LayoutInflater inflater = (LayoutInflater) getSystemService(LAYOUT_INFLATER_SERVICE);
        overlayView = inflater.inflate(R.layout.touchcontrol_overlay, null);

        // Set layout params for overlayView to control positioning and sizing
        FrameLayout.LayoutParams layoutParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
        );
        overlayView.setLayoutParams(layoutParams);
        // Add overlay view to the main layout (you may need to add it to a container like FrameLayout)
        ViewGroup view = (ViewGroup) getContentView();
        view.addView(overlayView);

        final ViewGroup buttonGroup = overlayView.findViewById(R.id.button_group);

        buttonA = overlayView.findViewById(R.id.buttonA);
        buttonB = overlayView.findViewById(R.id.buttonB);
        buttonX = overlayView.findViewById(R.id.buttonX);
        buttonY = overlayView.findViewById(R.id.buttonY);

        buttonDpadUp = overlayView.findViewById(R.id.buttonDpadUp);
        buttonDpadDown = overlayView.findViewById(R.id.buttonDpadDown);
        buttonDpadLeft = overlayView.findViewById(R.id.buttonDpadLeft);
        buttonDpadRight = overlayView.findViewById(R.id.buttonDpadRight);

        buttonLB = overlayView.findViewById(R.id.buttonLB);
        buttonRB = overlayView.findViewById(R.id.buttonRB);
        buttonZ = overlayView.findViewById(R.id.buttonZ);

        buttonStart = overlayView.findViewById(R.id.buttonStart);
        buttonBack = overlayView.findViewById(R.id.buttonBack);

        buttonToggle = overlayView.findViewById(R.id.buttonToggle);

        // Initialize joysticks and joystick knobs from the inflated layout
        leftJoystick = overlayView.findViewById(R.id.left_joystick);
        leftJoystickKnob = overlayView.findViewById(R.id.left_joystick_knob);

        FrameLayout rightScreenArea = overlayView.findViewById(R.id.right_screen_area);

        // Set OnTouchListeners for the Xbox controller buttons
        addTouchListener(buttonA, ControllerButtons.BUTTON_A); // SDL Button 0 (A)
        addTouchListener(buttonB, ControllerButtons.BUTTON_B); // SDL Button 1 (B)
        addTouchListener(buttonX, ControllerButtons.BUTTON_X); // SDL Button 2 (X)
        addTouchListener(buttonY, ControllerButtons.BUTTON_Y); // SDL Button 3 (Y)

        setupCButtons(buttonDpadUp, ControllerButtons.AXIS_RY, 1); // SDL Button 10 (D-Pad Up)
        setupCButtons(buttonDpadDown, ControllerButtons.AXIS_RY , -1); // SDL Button 11 (D-Pad Down)
        setupCButtons(buttonDpadLeft, ControllerButtons.AXIS_RX, 1); // SDL Button 12 (D-Pad Left)
        setupCButtons(buttonDpadRight, ControllerButtons.AXIS_RX, -1); // SDL Button 13 (D-Pad Right)

        addTouchListener(buttonLB, ControllerButtons.BUTTON_LB); // SDL Button 4 (LB)
        addTouchListener(buttonRB, ControllerButtons.BUTTON_RB); // SDL Button 5 (RB)
        addTouchListener(buttonZ, ControllerButtons.AXIS_RT); // SDL Button 5 (Z)

        addTouchListener(buttonStart, ControllerButtons.BUTTON_START); // SDL Button 7 (Start)
        addTouchListener(buttonBack, ControllerButtons.BUTTON_BACK); // SDL Button 6 (Back)


        // Setup joystick movement
        setupJoystick(leftJoystick, leftJoystickKnob, true); // Left joystick

        setupLookAround(rightScreenArea);

        setupToggleButton(buttonToggle,buttonGroup);

    }

    private ViewGroup touchButtonGroup;
    private boolean controlsHiddenByUser = false;
    private boolean controlsHiddenForGamepad = false;

    private void setupToggleButton(Button button, ViewGroup uiGroup){
        touchButtonGroup = uiGroup;
        controlsHiddenByUser = preferences.getBoolean("controlsVisible", false); // the key holds "hidden"
        button.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                controlsHiddenByUser = !controlsHiddenByUser;
                controlsHiddenForGamepad = false;
                preferences.edit().putBoolean("controlsVisible", controlsHiddenByUser).apply();
                updateTouchControlsVisibility();
            }
        });
        // A gamepad's buttons must reach the game, never the overlay's views.
        disableFocus(overlayView);
        controlsHiddenForGamepad = hasPhysicalGamepad();
        updateTouchControlsVisibility();
        InputManager inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        inputManager.registerInputDeviceListener(new InputManager.InputDeviceListener() {
            @Override
            public void onInputDeviceAdded(int deviceId) {
                if (isPhysicalGamepad(InputDevice.getDevice(deviceId))) {
                    setHiddenForGamepad(true);
                }
            }

            @Override
            public void onInputDeviceRemoved(int deviceId) {
                if (!hasPhysicalGamepad()) {
                    setHiddenForGamepad(false);
                }
            }

            @Override
            public void onInputDeviceChanged(int deviceId) {
            }
        }, null);
    }

    private static void disableFocus(View view) {
        view.setFocusable(false);
        view.setFocusableInTouchMode(false);
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                disableFocus(group.getChildAt(i));
            }
        }
    }

    private static boolean isPhysicalGamepad(InputDevice device) {
        if (device == null || device.isVirtual()) {
            return false;
        }
        int sources = device.getSources();
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private static boolean hasPhysicalGamepad() {
        for (int id : InputDevice.getDeviceIds()) {
            if (isPhysicalGamepad(InputDevice.getDevice(id))) {
                return true;
            }
        }
        return false;
    }

    private static boolean isGamepadEvent(InputEvent event) {
        return event.isFromSource(InputDevice.SOURCE_GAMEPAD) || event.isFromSource(InputDevice.SOURCE_JOYSTICK);
    }

    private void setHiddenForGamepad(boolean hidden) {
        if (controlsHiddenForGamepad != hidden) {
            controlsHiddenForGamepad = hidden;
            updateTouchControlsVisibility();
        }
    }

    private void updateTouchControlsVisibility() {
        if (touchButtonGroup == null) {
            return;
        }
        // The touch controls are only for playing: not over the settings menu, the title menu or the opening.
        boolean menuOpen = !TouchAreaEnabled || TitleMenuShown || introActive();
        boolean hidden = controlsHiddenByUser || controlsHiddenForGamepad || menuOpen;
        touchButtonGroup.setVisibility(hidden ? View.INVISIBLE : View.VISIBLE);
        if (buttonToggle != null) {
            buttonToggle.setVisibility(menuOpen ? View.INVISIBLE : View.VISIBLE);
        }
        // Clear anything the overlay was holding so the game does not see a stuck button or stick.
        if (hidden) {
            for (int button : new int[] { ControllerButtons.BUTTON_A, ControllerButtons.BUTTON_B,
                    ControllerButtons.BUTTON_X, ControllerButtons.BUTTON_Y, ControllerButtons.BUTTON_LB,
                    ControllerButtons.BUTTON_RB, ControllerButtons.BUTTON_START, ControllerButtons.BUTTON_BACK }) {
                setButton(button, false);
            }
            setButton(ControllerButtons.AXIS_RT, false);
            setAxis(ControllerButtons.AXIS_LX, (short) 0);
            setAxis(ControllerButtons.AXIS_LY, (short) 0);
            setAxis(ControllerButtons.AXIS_RX, (short) 0);
            setAxis(ControllerButtons.AXIS_RY, (short) 0);
        }
    }

    // A physical gamepad in use hides the touch controls; touching the screen in game brings them back.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (SetupOverlay.dispatchKey(event)) {
            return true;
        }
        if (intro != null && intro.isActive() && intro.onKey(event)) {
            return true;
        }
        if (isGamepadEvent(event)) {
            setHiddenForGamepad(true);
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (SetupOverlay.dispatchMotion(event)) {
            return true;
        }
        if (intro != null && intro.isActive()) {
            return true; // sticks do nothing under the opening video
        }
        if (isGamepadEvent(event)) {
            setHiddenForGamepad(true);
        }
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN && controlsHiddenForGamepad && TouchAreaEnabled) {
            setHiddenForGamepad(false);
        }
        return super.dispatchTouchEvent(event);
    }

    // Function to set a touch listener for each button
    private void addTouchListener(Button button, int buttonNum) {
        button.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent event) {
                if (introActive()) {
                    // Under the opening the buttons only skip it (held), and nothing reaches the game.
                    int action = event.getActionMasked();
                    if (action == MotionEvent.ACTION_DOWN) {
                        button.setPressed(true);
                        intro.holdStart();
                    } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
                        button.setPressed(false);
                        intro.holdEnd();
                    }
                    return true;
                }
                switch (event.getAction()) {
                    case MotionEvent.ACTION_DOWN:
                        setButton(buttonNum, true);
                        button.setPressed(true);
                        return true;
                    case MotionEvent.ACTION_UP:
                        setButton(buttonNum, false);
                        button.setPressed(false);
                        return true;
                    case MotionEvent.ACTION_CANCEL:
                        setButton(buttonNum, false);
                        return true;
                }
                return false;
            }
        });
    }

    private void setupCButtons(Button button, int buttonNum, int direction) {
        button.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent event) {
                switch (event.getAction()) {
                    case MotionEvent.ACTION_DOWN:
                        setAxis(buttonNum, direction<0 ? Short.MAX_VALUE : Short.MIN_VALUE);
                        button.setPressed(true);
                        return true;
                    case MotionEvent.ACTION_UP:
                        setAxis(buttonNum, (short) 0);
                        button.setPressed(false);
                        return true;
                    case MotionEvent.ACTION_CANCEL:
                        setAxis(buttonNum, (short) 0);
                        return true;
                }
                return false;
            }
        });
    }

    volatile boolean TouchAreaEnabled = true;

    // Called from the game thread when the 2Ship menu opens or closes. While it is open, the touch controls are
    // hidden so every tap reaches the menu instead of the buttons drawn over it.
    void DisableTouchArea(){
        TouchAreaEnabled = false;
        runOnUiThread(this::updateTouchControlsVisibility);
    }
    void EnableTouchArea(){
        TouchAreaEnabled = true;
        runOnUiThread(this::updateTouchControlsVisibility);
    }

    // Called from the game thread while the new title menu is on screen. The touch controls are hidden there (the
    // menu is driven by touching its own buttons, or a controller) and come back once a file is loaded and played.
    volatile boolean TitleMenuShown = false;
    void SetTitleMenuShown(boolean shown){
        TitleMenuShown = shown;
        if (shown) {
            setCameraState(0, 0.0f);
            setCameraState(1, 0.0f);
        }
        runOnUiThread(this::updateTouchControlsVisibility);
    }

    // IntroPlayer: the opening came on or went away (no touch controls over it).
    void IntroChanged() {
        runOnUiThread(this::updateTouchControlsVisibility);
    }

    private void setupLookAround(FrameLayout rightScreenArea) {
        rightScreenArea.setOnTouchListener(new View.OnTouchListener() {
            private float lastX = 0;
            private float lastY = 0;
            private boolean isTouching = false;

            @Override
            public boolean onTouch(View v, MotionEvent event) {
                if (TitleMenuShown || introActive()) {
                    isTouching = false;
                    return false; // the tap goes on to the title menu, or the opening under the controls
                }
                switch (event.getAction()) {
                    case MotionEvent.ACTION_DOWN:
                        // Start tracking the finger's position
                        lastX = event.getX();
                        lastY = event.getY();
                        isTouching = true;
                        break;

                    case MotionEvent.ACTION_MOVE:
                        if (isTouching) {
                            // Calculate the change in position (delta)
                            float deltaX = event.getX() - lastX;
                            float deltaY = event.getY() - lastY;

                            // Update the last position
                            lastX = event.getX();
                            lastY = event.getY();

                            // Increase sensitivity by using a larger multiplier
                            // Adjust these multipliers to suit your needs
                            float sensitivityMultiplier = 15; // Higher value for more sensitivity
                            float rx = (deltaX * sensitivityMultiplier);
                            float ry = (deltaY * sensitivityMultiplier);

                            // Send the mapped values to the joystick axes
                            setCameraState(0, rx); // Right stick X axis
                            setCameraState(1, ry); // Right stick Y axis
                        }
                        break;

                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        // Stop tracking the finger's position and reset joystick input
                        isTouching = false;
                        setCameraState(0, 0.0f); // Reset right stick X axis
                        setCameraState(1, 0.0f); // Reset right stick Y axis
                        break;
                }
                return TouchAreaEnabled; // Event full handled
            }
        });
    }





    // Function to set joystick movement with reset to center when not touched
    private void setupJoystick(FrameLayout joystickLayout, ImageView joystickKnob, boolean isLeft) {
        joystickLayout.post(() -> {
            // Calculate the joystick center once, before any events
            final float joystickCenterX = joystickLayout.getWidth() / 2f;
            final float joystickCenterY = joystickLayout.getHeight() / 2f;

            joystickLayout.setOnTouchListener(new View.OnTouchListener() {
                @Override
                public boolean onTouch(View v, MotionEvent event) {
                    switch (event.getAction()) {
                        case MotionEvent.ACTION_DOWN:
                        case MotionEvent.ACTION_MOVE:
                            // Calculate the joystick movement and move the knob
                            float deltaX = event.getX() - joystickCenterX;
                            float deltaY = event.getY() - joystickCenterY;

                            // Clamp the joystick movement to prevent it from going outside the area
                            float maxRadius = joystickLayout.getWidth() / 2f - joystickKnob.getWidth() / 2f;
                            float distance = (float) Math.sqrt(deltaX * deltaX + deltaY * deltaY);
                            if (distance > maxRadius) {
                                float scale = maxRadius / distance;
                                deltaX *= scale;
                                deltaY *= scale;
                            }

                            joystickKnob.setX(joystickCenterX + deltaX - joystickKnob.getWidth() / 2f);
                            joystickKnob.setY(joystickCenterY + deltaY - joystickKnob.getHeight() / 2f);

                            // Send joystick values to native C code
                            short x = (short) (deltaX / maxRadius * Short.MAX_VALUE);
                            short y = (short) (deltaY / maxRadius * Short.MAX_VALUE);

                            // Send X-axis and Y-axis values
                            setAxis(isLeft ? ControllerButtons.AXIS_LX : ControllerButtons.AXIS_RX, x); // X-axis
                            setAxis(isLeft ? ControllerButtons.AXIS_LY : ControllerButtons.AXIS_RY, y); // Y-axis
                            break;

                        case MotionEvent.ACTION_UP:
                        case MotionEvent.ACTION_CANCEL:
                            // Reset joystick knob to the center position (ensure it's placed correctly)
                            joystickKnob.setX(joystickCenterX - joystickKnob.getWidth() / 2f);
                            joystickKnob.setY(joystickCenterY - joystickKnob.getHeight() / 2f);

                            // Reset joystick values to 0 when released or canceled
                            setAxis(isLeft ? ControllerButtons.AXIS_LX : ControllerButtons.AXIS_RX, (short) 0); // X-axis
                            setAxis(isLeft ? ControllerButtons.AXIS_LY : ControllerButtons.AXIS_RY, (short) 0); // Y-axis
                            break;
                    }
                    return true;
                }
            });
        });



}



}
