// Fake Java VM for the recompiled game: JNIEnv/JavaVM function tables live in guest
// memory and point at host implementations; Java objects are host-side records behind
// opaque handles. Java methods the game calls are emulated by registered C++ functions;
// unknown ones are logged once and return 0/null.
#pragma once
#include "runtime.h"

#include <functional>
#include <string>
#include <vector>

namespace jni {

using Handle = uint32_t;  // jobject / jclass / jstring ... as seen by the guest (0 = null)

// A Java value: the 32-bit members overlay the low half of the 64-bit ones (raw = a word read
// as i), which on a big-endian host (the GameCube) is the second half.
union Value {
    uint64_t raw;
    int64_t j;
    double d;
    struct {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        uint32_t high_;
#endif
        union {
            int32_t i;
            float f;
            Handle l;
        };
    };
};

struct Call {
    Handle self;               // `this`, or the class for static methods
    std::vector<Value> args;
    const std::string &cls;
    const std::string &name;
    const std::string &sig;
};

using Method = std::function<Value(Call &)>;

void init();
uint32_t env();  // guest JNIEnv*
void set_screen_size(int width, int height);  // reported to the game as the device resolution
uint32_t vm();   // guest JavaVM*

// Java-side emulation registry. `key` is "pkg/Class.method(sig)ret", e.g.
// "android/os/Build$VERSION.SDK_INT" for fields, "com/x/Y.foo(I)V" for methods.
void register_method(const std::string &key, Method m);
void register_static_field(const std::string &key, Value v);

// Object helpers for emulation code.
Handle new_string(const std::string &s);
std::string string_of(Handle h);
Handle find_class(const std::string &name);
Handle new_object(const std::string &cls);
Handle new_int_array(const std::vector<int32_t> &values);
// A primitive array's storage: guest address, element count and type char (Z B C S I J F D).
bool array_data(Handle h, uint32_t &guest_buf, uint32_t &length, char &elem);

// Java objects alive (classes included); the periodic memory log line.
size_t live_objects();

// Invoke a native export Java_* with (env, clazz, args...) on the current guest thread.
uint32_t call_native(const char *export_name, const std::string &cls, std::initializer_list<uint32_t> args);

}  // namespace jni
