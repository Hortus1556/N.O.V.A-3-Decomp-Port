// Java-side emulation: the Android framework values and Gameloft Java helpers that the
// native code calls through JNI. Anything not listed here is logged by jni.cpp and
// returns 0/null; entries are added as the boot log shows they matter.
#include "jni.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>

namespace jni {

namespace {

int g_screen_w = 1280, g_screen_h = 720;

Value str(const std::string &s) { Value v{}; v.l = new_string(s); return v; }
Value i32(int32_t x) { Value v{}; v.i = x; return v; }

// ---- android.os.Bundle: typed key/value maps kept host-side ------------------------------
struct Entry {
    char kind;          // 'I' int, 'J' long, 'Z' boolean, 'S' string
    Value v;            // numeric value
    std::string s;      // string value
};
std::mutex g_bundle_lock;
std::unordered_map<Handle, std::map<std::string, Entry>> g_bundles;

std::map<std::string, Entry> &bundle(Handle h) { return g_bundles[h]; }

void bundle_put(Call &c, char kind) {
    std::lock_guard<std::mutex> lock(g_bundle_lock);
    Entry e{kind, c.args[1], kind == 'S' ? string_of(c.args[1].l) : std::string()};
    bundle(c.self)[string_of(c.args[0].l)] = e;
}
Value bundle_get(Call &c) {
    std::lock_guard<std::mutex> lock(g_bundle_lock);
    auto &b = bundle(c.self);
    auto it = b.find(string_of(c.args[0].l));
    if (it == b.end()) return c.args.size() > 1 ? c.args[1] : Value{};  // (key, default) overloads
    if (it->second.kind == 'S') return str(it->second.s);
    return it->second.v;
}

// ---- SharedPreferences store (npPrefName/npKey -> typed value), persisted as text ----------
std::map<std::string, Entry> g_prefs;
std::string prefs_file() {
    return rt::host_path("/data/data/com.gameloft.android.ANMP.GloftNAHM/shared_prefs/nova3_prefs.txt");
}
void load_prefs() {
    std::ifstream f(prefs_file());
    std::string line;
    while (std::getline(f, line)) {
        size_t a = line.find('\t'), b = line.find('\t', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        Entry e{line[a + 1], Value{}, ""};
        std::string val = line.substr(b + 1);
        if (e.kind == 'S') e.s = val;
        else e.v.raw = uint64_t(std::stoll(val));
        g_prefs[line.substr(0, a)] = e;
    }
}
void save_prefs() {
    std::string path = prefs_file();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::trunc);
    for (auto &[k, e] : g_prefs) {
        f << k << '\t' << e.kind << '\t';
        if (e.kind == 'S') f << e.s;
        else if (e.kind == 'J') f << int64_t(e.v.raw);
        else f << e.v.i;
        f << '\n';
    }
}
// ---- DataSharing (Gameloft's values shared between its apps), persisted as text ------------
// Gaia keeps the anonymous account ID (GLUID) here. Without it the ID was generated anew
// from the clock on every request, and every save (stamped with it, Application::
// EncryptAndSave / DecryptAndLoad) was refused when loaded again: no Continue, progress lost.
std::mutex g_shared_lock;
std::map<std::string, std::string> g_shared;
bool g_shared_loaded = false;
std::string shared_file() {
    return rt::host_path("/data/data/com.gameloft.android.ANMP.GloftNAHM/shared_prefs/data_sharing.txt");
}
std::map<std::string, std::string> &shared() {  // g_shared_lock held
    if (!g_shared_loaded) {
        g_shared_loaded = true;
        std::ifstream f(shared_file());
        std::string line;
        while (std::getline(f, line)) {
            size_t tab = line.find('\t');
            if (tab != std::string::npos) g_shared[line.substr(0, tab)] = line.substr(tab + 1);
        }
    }
    return g_shared;
}
void save_shared() {  // g_shared_lock held
    std::string path = shared_file();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::trunc);
    for (auto &[k, v] : g_shared) f << k << '\t' << v << '\n';
}

std::string pref_key(std::map<std::string, Entry> &b) {
    auto get = [&](const char *k) { auto it = b.find(k); return it == b.end() ? std::string() : it->second.s; };
    return get("npPrefName") + "/" + get("npKey");
}

}  // namespace

void set_screen_size(int w, int h) {
    g_screen_w = w;
    g_screen_h = h;
}

void register_java() {
    // android.os.Build: present as a generic high-end device
    const std::pair<const char *, const char *> build[] = {
        {"MANUFACTURER", "nova3pc"}, {"MODEL", "Windows PC"}, {"DEVICE", "nova3pc"}, {"PRODUCT", "nova3pc"},
        {"BRAND", "nova3pc"}, {"BOARD", "pc"}, {"HARDWARE", "pc"}, {"ID", "JDQ39"}, {"SERIAL", "0000000000"},
        {"FINGERPRINT", "nova3pc/nova3pc/nova3pc:4.2.2/JDQ39/1:user/release-keys"}, {"CPU_ABI", "armeabi-v7a"},
        {"CPU_ABI2", "armeabi"}, {"DISPLAY", "JDQ39"}, {"HOST", "localhost"}, {"TAGS", "release-keys"},
        {"TYPE", "user"}, {"USER", "nova3"}, {"BOOTLOADER", "unknown"}, {"RADIO", "unknown"},
    };
    for (auto &[k, v] : build) register_static_field(std::string("android/os/Build.") + k, str(v));
    register_static_field("android/os/Build$VERSION.SDK_INT", i32(17));
    register_static_field("android/os/Build$VERSION.RELEASE", str("4.2.2"));
    register_static_field("android/os/Build$VERSION.SDK", str("17"));
    register_static_field("android/os/Build$VERSION.INCREMENTAL", str("1"));
    register_static_field("android/os/Build$VERSION.CODENAME", str("REL"));

    // GL2JNILib.setupPaths(): the native side asks Java for paths once glf::App exists;
    // Java answers by calling native setPaths(obbFolder, sdFolder, filesDir, cacheDir).
    const std::string pkg = "com/gameloft/android/ANMP/GloftNAHM/";
    register_method(pkg + "GL2JNILib.setupPaths()V", [pkg](Call &) {
        const std::string app = "/data/data/com.gameloft.android.ANMP.GloftNAHM";
        call_native("Java_com_gameloft_android_ANMP_GloftNAHM_GL2JNILib_setPaths", pkg + "GL2JNILib",
                    {new_string("/sdcard/Android/obb/com.gameloft.android.ANMP.GloftNAHM"),
                     new_string("/sdcard/gameloft/games/GloftNAHM"), new_string(app + "/files"),
                     new_string(app + "/cache")});
        return Value{};
    });

    // Game-side Java helpers (GLUtils.SUtils / GLUtils.Device / SendInfo / GL2JNILib).
    // Identity values are fixed, anonymous placeholders; the network is reported absent.
    auto ret_str = [&](const std::string &key, std::string value) {
        register_method(pkg + key, [value](Call &) { return str(value); });
    };
    auto ret_int = [&](const std::string &key, int32_t value) {
        register_method(pkg + key, [value](Call &) { return i32(value); });
    };
    auto ret_void = [&](const std::string &key) { register_method(pkg + key, [](Call &) { return Value{}; }); };
    const std::string sd = "/sdcard/gameloft/games/GloftNAHM";
    const std::string save = "/data/data/com.gameloft.android.ANMP.GloftNAHM/files";
    const std::string zero_id = "00000000-0000-0000-0000-000000000000";
    // ConnectivityManager.TYPE_WIFI: local (LAN) multiplayer requires Wi-Fi. Online services
    // still fail cleanly because DNS lookups and outbound connections are not available.
    ret_int("GLUtils/SUtils.initCheckConnectionType()I", 1);
    ret_str("GLUtils/SUtils.getGameName()Ljava/lang/String;", "GloftNAHM");
    ret_str("GLUtils/SUtils.getInjectedIGP()Ljava/lang/String;", "");
    ret_str("GLUtils/SUtils.getInjectedSerialKey()Ljava/lang/String;", "");
    ret_str("GLUtils/SUtils.getSDFolder()Ljava/lang/String;", sd);
    ret_str("GLUtils/SUtils.getSaveFolder()Ljava/lang/String;", save);
    ret_str("GLUtils/Device.d1()Ljava/lang/String;", "");
    ret_str("GLUtils/Device.getAndroidId()Ljava/lang/String;", "0000000000000000");
    ret_str("GLUtils/Device.getSerial()Ljava/lang/String;", "0000000000");
    ret_str("GLUtils/Device.getSerialNo()Ljava/lang/String;", "0000000000");
    ret_str("GLUtils/Device.getDeviceFirmware()Ljava/lang/String;", "4.2.2");
    ret_str("GLUtils/Device.getMacAddress()Ljava/lang/String;", "00:00:00:00:00:00");
    ret_str("GLUtils/Device.getDeviceIMEI()Ljava/lang/String;", "000000000000000");
    ret_str("GLUtils/Device.getHDIDFV()Ljava/lang/String;", zero_id);
    ret_str("GLUtils/Device.getHDIDFVVersion()Ljava/lang/String;", "1");
    ret_str("GLUtils/Device.getGLDID()Ljava/lang/String;", zero_id);
    ret_str("GLUtils/Device.getDeviceName()Ljava/lang/String;", "Windows PC");
    ret_str("GLUtils/Device.getPhoneManufacturer()Ljava/lang/String;", "nova3pc");
    ret_str("GLUtils/Device.getPhoneModel()Ljava/lang/String;", "Windows PC");
    ret_str("GLUtils/Device.retrieveCPUSerial()Ljava/lang/String;", "0");
    ret_str("GLUtils/Device.getPhoneDevice()Ljava/lang/String;", "nova3pc");
    ret_str("GLUtils/Device.getPhoneProduct()Ljava/lang/String;", "nova3pc");
    ret_str("GLUtils/Device.getGoogleAdId()Ljava/lang/String;", "");
    ret_int("GLUtils/Device.getGoogleAdIdStatus()I", 0);
    ret_str("SendInfo.getSDFolder()Ljava/lang/String;", sd);
    ret_str("SendInfo.getSaveFolder()Ljava/lang/String;", save);
    register_method(pkg + "SendInfo.getGLUID()[I", [](Call &) { Value v{}; v.l = new_int_array({0, 0, 0, 0}); return v; });
    ret_str("SendInfo.getPhoneCarrier()Ljava/lang/String;", "");
    ret_str("SendInfo.getLocaleCountry()Ljava/lang/String;", "US");
    ret_str("SendInfo.getLocaleLanguage()Ljava/lang/String;", "en");
    ret_str("SendInfo.getManufacturerModel()Ljava/lang/String;", "nova3pc Windows PC");
    ret_str("SendInfo.getGLDID()Ljava/lang/String;", zero_id);
    ret_str("SendInfo.getMacAddress()Ljava/lang/String;", "00:00:00:00:00:00");
    ret_str("GLUtils/SUtils.GetApkPath()Ljava/lang/String;", "/data/app/com.gameloft.android.ANMP.GloftNAHM-1.apk");
    ret_str("GLUtils/SUtils.getMetaDataValue(Ljava/lang/String;)Ljava/lang/String;", "");

    // android.os.Bundle
    register_method("android/os/Bundle.<init>()V", [](Call &c) {
        std::lock_guard<std::mutex> lock(g_bundle_lock);
        bundle(c.self);
        return Value{};
    });
    for (auto [name, kind] : {std::pair<const char *, char>{"putInt(Ljava/lang/String;I)V", 'I'},
                              {"putLong(Ljava/lang/String;J)V", 'J'}, {"putBoolean(Ljava/lang/String;Z)V", 'Z'},
                              {"putString(Ljava/lang/String;Ljava/lang/String;)V", 'S'}}) {
        char k = kind;
        register_method(std::string("android/os/Bundle.") + name, [k](Call &c) { bundle_put(c, k); return Value{}; });
    }
    for (const char *name : {"getInt(Ljava/lang/String;)I", "getInt(Ljava/lang/String;I)I", "getLong(Ljava/lang/String;)J",
                             "getLong(Ljava/lang/String;J)J", "getBoolean(Ljava/lang/String;)Z",
                             "getBoolean(Ljava/lang/String;Z)Z", "getString(Ljava/lang/String;)Ljava/lang/String;",
                             "getString(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"})
        register_method(std::string("android/os/Bundle.") + name, bundle_get);
    register_method("android/os/Bundle.containsKey(Ljava/lang/String;)Z", [](Call &c) {
        std::lock_guard<std::mutex> lock(g_bundle_lock);
        return i32(bundle(c.self).count(string_of(c.args[0].l)) ? 1 : 0);
    });

    // SharedPreferences bridge (GameUtils::setSharedPrefence / getSharedPrefence)
    load_prefs();
    register_method(pkg + "GLUtils/SUtils.nativeSetPreference(Landroid/os/Bundle;)V", [](Call &c) {
        std::lock_guard<std::mutex> lock(g_bundle_lock);
        auto &b = bundle(c.args[0].l);
        auto data = b.find("npData");
        if (data != b.end()) {
            g_prefs[pref_key(b)] = data->second;
            save_prefs();
        }
        return Value{};
    });
    register_method(pkg + "GLUtils/SUtils.nativeGetPreference(Landroid/os/Bundle;)Landroid/os/Bundle;", [](Call &c) {
        std::lock_guard<std::mutex> lock(g_bundle_lock);
        auto &in = bundle(c.args[0].l);
        Handle out = new_object("android/os/Bundle");
        auto stored = g_prefs.find(pref_key(in));
        auto def = in.find("npDefaultValue");
        if (stored != g_prefs.end()) bundle(out)["npResult"] = stored->second;
        else if (def != in.end()) bundle(out)["npResult"] = def->second;
        Value v{};
        v.l = out;
        return v;
    });

    ret_str("GLUtils/Device.retrieveDeviceCarrier()Ljava/lang/String;", "");
    ret_str("GLUtils/Device.retrieveDeviceCountry()Ljava/lang/String;", "US");
    ret_str("GLUtils/Device.retrieveDeviceLanguage()Ljava/lang/String;", "en");
    register_method(pkg + "GLUtils/SUtils.retrieveBarrels()[I", [](Call &) { Value v{}; v.l = new_int_array({}); return v; });
    // Report a high-end device so the game picks its best quality settings.
    // the game's CPU profile (GameOptions.json "cores" / "cpufmax"): the 3DS is one 804 MHz core
    register_method(pkg + "GL2JNILib.GetPhoneCPUFreq()F", [](Call &) { Value v{}; v.f = rt::low_end_device() ? 804.0f : 2000.0f; return v; });
    ret_int("GL2JNILib.GetPhoneCPUCores()I", rt::low_end_device() ? 1 : 4);
    register_method(pkg + "GL2JNILib.GetPhoneMemory()F", [](Call &) { Value v{}; v.f = 2048.0f; return v; });
    ret_void("GL2JNILib.setViewSettings(IIIII)V");
    ret_int("GL2JNILib.IsNewDay()Z", 0);
    ret_str("GL2JNILib.GetSDFolder()Ljava/lang/String;", sd);
    ret_void("GL2JNILib.KeepScreenOn(I)V");
    ret_str("PushNotification/SimplifiedAndroidUtils.IsAppLaunchedFromPN()Ljava/lang/String;", "");
    ret_str("GL2JNILib.GetPhoneLanguage()Ljava/lang/String;", "en");
    register_method(pkg + "GL2JNILib.GetInt(Ljava/lang/String;I)I", [](Call &c) { return c.args[1]; });  // default
    register_method(pkg + "DataSharing.isSharedValue(Ljava/lang/String;)Z", [](Call &c) {
        std::lock_guard<std::mutex> lock(g_shared_lock);
        return i32(shared().count(string_of(c.args[0].l)) ? 1 : 0);
    });
    register_method(pkg + "DataSharing.getSharedValue(Ljava/lang/String;)Ljava/lang/String;", [](Call &c) {
        std::lock_guard<std::mutex> lock(g_shared_lock);
        auto it = shared().find(string_of(c.args[0].l));
        return str(it == shared().end() ? std::string() : it->second);
    });
    register_method(pkg + "DataSharing.setSharedValue(Ljava/lang/String;Ljava/lang/String;)V", [](Call &c) {
        std::lock_guard<std::mutex> lock(g_shared_lock);
        std::string value = string_of(c.args[1].l);
        for (char &ch : value)
            if (ch == '\n' || ch == '\t') ch = ' ';  // (one line per value; base64 / JSON never has them)
        shared()[string_of(c.args[0].l)] = value;
        save_shared();
        return Value{};
    });
    register_method(pkg + "DataSharing.deleteSharedValue(Ljava/lang/String;)V", [](Call &c) {
        std::lock_guard<std::mutex> lock(g_shared_lock);
        shared().erase(string_of(c.args[0].l));
        save_shared();
        return Value{};
    });
    ret_str("GL2JNILib.GetGLLiveUsername()Ljava/lang/String;", "");
    ret_str("GL2JNILib.GetGLLivePassword()Ljava/lang/String;", "");
    // GameOptions asks the device for its screen resolution.
    register_method("com/gameloft/gameoptions/DeviceOptions.getResolutionX()I", [](Call &) { return i32(g_screen_w); });
    register_method("com/gameloft/gameoptions/DeviceOptions.getResolutionY()I", [](Call &) { return i32(g_screen_h); });
    // Social SDKs are offline: never logged in.
    const std::string social = "com/gameloft/GLSocialLib/";
    register_method(social + "facebook/FacebookAndroidGLSocialLib.Init(Ljava/lang/String;)V", [](Call &) { return Value{}; });
    register_method(social + "facebook/FacebookAndroidGLSocialLib.IsLoggedIn()Z", [](Call &) { return i32(0); });
    register_method(social + "GameAPI/GameAPIAndroidGLSocialLib.InitGameAPI(ZZZ)V", [](Call &) { return Value{}; });
    register_method(social + "GameAPI/GameAPIAndroidGLSocialLib.IsLoggedIn()Z", [](Call &) { return i32(0); });
    register_method(pkg + "GL2JNILib.GetWindowWidth()I", [](Call &) { return i32(g_screen_w); });
    register_method(pkg + "GL2JNILib.GetWindowHeight()I", [](Call &) { return i32(g_screen_h); });
    ret_void("GL2JNILib.enableGyroscope(ZF)V");
    ret_void("GL2JNILib.SetIsActionPhase(I)V");
    ret_void("GL2JNILib.createView()V");
    ret_void("GL2JNILib.enableAccelerometer(ZF)V");

    // java.lang.String helpers the native code may use on Java strings
    register_method("java/lang/String.length()I", [](Call &c) { return i32(int32_t(string_of(c.self).size())); });
}

}  // namespace jni
