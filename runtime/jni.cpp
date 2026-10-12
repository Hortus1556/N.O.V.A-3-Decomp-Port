// Fake JNI: JavaVM/JNIEnv function tables in guest memory + host-side Java objects.
#include "jni.h"

#include <mutex>
#include <set>
#include <unordered_map>

namespace jni {
namespace {

using rt::Args;
using u32 = uint32_t;

// ---- objects ------------------------------------------------------------------------
struct Obj {
    enum Kind { Class, String, Array, Instance, Buffer } kind;
    std::string cls;                // class name (for Class: the class itself)
    std::string str;                // String contents (modified UTF-8 ~ UTF-8)
    char elem = 0;                  // Array element type ('Z','B',...,'L')
    u32 buf = 0, len = 0;           // primitive array storage (guest) / direct buffer
    std::vector<Handle> elems;      // object arrays
    std::unordered_map<std::string, Value> fields;
};

constexpr u32 kHandleBase = 0xf8000000u, kMethodBase = 0xf4000000u, kFieldBase = 0xf6000000u;

std::recursive_mutex g_lock;
std::vector<Obj *> g_objs = {nullptr};
std::vector<u32> g_free_slots;
std::unordered_map<std::string, Handle> g_classes;

Handle add(Obj *o) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    u32 i;
    if (!g_free_slots.empty()) { i = g_free_slots.back(); g_free_slots.pop_back(); g_objs[i] = o; }
    else { i = u32(g_objs.size()); g_objs.push_back(o); }
    return kHandleBase + i * 4;
}
Obj *get(Handle h) {
    if (!h) return nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    u32 i = (h - kHandleBase) / 4;
    if (h < kHandleBase || i >= g_objs.size() || !g_objs[i]) {
        rt::log("JNI: invalid object handle %08x", h);
        return nullptr;
    }
    return g_objs[i];
}
void release(Handle h);
}  // namespace

size_t live_objects() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_objs.size() - 1 - g_free_slots.size();
}

namespace {
void release(Handle h) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Obj *o = get(h);
    if (!o || o->kind == Obj::Class) return;  // classes are permanent
    u32 i = (h - kHandleBase) / 4;
    if (o->kind == Obj::Array && o->buf) rt::gfree(o->buf);
    delete o;
    g_objs[i] = nullptr;
    g_free_slots.push_back(i);
}

// ---- method / field ids ------------------------------------------------------------------
struct Member {
    std::string cls, name, sig;
    bool is_static;
};
std::vector<Member> g_methods, g_fields;
std::unordered_map<std::string, Method> g_impls;
std::unordered_map<std::string, Value> g_static_fields;
std::set<std::string> g_warned;

u32 member_id(std::vector<Member> &table, u32 base, Member m) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (u32 i = 0; i < table.size(); i++)
        if (table[i].cls == m.cls && table[i].name == m.name && table[i].sig == m.sig) return base + 4 + i * 4;
    table.push_back(std::move(m));
    return base + u32(table.size()) * 4;
}
const Member &member(std::vector<Member> &table, u32 base, u32 id) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    u32 i = (id - base) / 4 - 1;
    if (id <= base || i >= table.size()) rt::fatal("JNI: invalid member id %08x", id);
    return table[i];
}

void warn_once(const std::string &what) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_warned.insert(what).second) rt::log("JNI: unimplemented %s", what.c_str());
}

// Parameter type chars of a JNI signature: Z B C S I J F D L (objects/arrays) ; and return type.
std::string param_types(const std::string &sig, char *ret) {
    std::string out;
    size_t i = 1;
    while (i < sig.size() && sig[i] != ')') {
        if (sig[i] == '[' || sig[i] == 'L') {  // arrays and objects are both references
            while (sig[i] == '[') i++;
            i = sig[i] == 'L' ? sig.find(';', i) + 1 : i + 1;
            out += 'L';
        } else {
            out += sig[i++];
        }
    }
    *ret = i + 1 < sig.size() ? sig[i + 1] : 'V';
    if (*ret == '[') *ret = 'L';
    return out;
}

enum class ArgMode { Variadic, VaList, Array };

std::vector<Value> read_args(Cpu *cpu, const std::string &types, ArgMode mode, u32 first_index) {
    std::vector<Value> v;
    if (mode == ArgMode::Array) {
        u32 p = first_index < 4 ? cpu->r[first_index] : LD32(cpu->r[13] + (first_index - 4) * 4);
        for (char t : types) {
            Value x{};
            switch (t) {
            case 'Z': x.i = LD8(p); break;
            case 'B': x.i = int8_t(LD8(p)); break;
            case 'C': x.i = LD16(p); break;
            case 'S': x.i = int16_t(LD16(p)); break;
            case 'F': x.raw = LD32(p); break;
            case 'J': case 'D': x.raw = LD64(p); break;
            default: x.raw = LD32(p);
            }
            v.push_back(x);
            p += 8;
        }
        return v;
    }
    Args a(cpu, first_index);
    if (mode == ArgMode::VaList) a = Args::va_list(cpu, a.word());
    for (char t : types) {
        Value x{};
        switch (t) {
        case 'F': { double d = a.get<double>(); x.f = float(d); break; }  // promoted to double
        case 'J': case 'D': x.raw = a.dword(); break;
        default: x.raw = a.word();
        }
        v.push_back(x);
    }
    return v;
}

void set_ret(Cpu *cpu, char t, Value v) {
    switch (t) {
    case 'V': break;
    case 'J': case 'D': cpu->r[0] = u32(v.raw); cpu->r[1] = u32(v.raw >> 32); break;
    case 'F': cpu->r[0] = u32(v.raw); break;
    case 'Z': cpu->r[0] = v.i ? 1 : 0; break;
    default: cpu->r[0] = u32(v.raw);
    }
}

std::string class_of(Handle h) {
    Obj *o = get(h);
    if (!o) return "?";
    return o->cls;
}

Value invoke(const Member &m, Handle self, std::vector<Value> args) {
    Method fn;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        auto it = g_impls.find(m.cls + "." + m.name + m.sig);
        if (it == g_impls.end()) it = g_impls.find("*." + m.name + m.sig);
        if (it != g_impls.end()) fn = it->second;
    }
    if (!fn) {
        warn_once(std::string(m.is_static ? "static " : "") + m.cls + "." + m.name + m.sig);
        return Value{};
    }
    Call c{self, std::move(args), m.cls, m.name, m.sig};
    return fn(c);
}

// ---- JNIEnv functions (guest calling convention: env is r0) ----------------------------------
u32 reg(Cpu *cpu, u32 i) { return i < 4 ? cpu->r[i] : LD32(cpu->r[13] + (i - 4) * 4); }

void fn_GetVersion(Cpu *cpu) { cpu->r[0] = 0x00010006; }
void fn_FindClass(Cpu *cpu) { cpu->r[0] = find_class(rt::str(cpu->r[1])); }
void fn_GetSuperclass(Cpu *cpu) { cpu->r[0] = find_class("java/lang/Object"); }
void fn_IsAssignableFrom(Cpu *cpu) { cpu->r[0] = 1; }
void fn_Throw(Cpu *cpu) { rt::log("JNI: Throw(%s) ignored", class_of(cpu->r[1]).c_str()); cpu->r[0] = 0; }
void fn_ThrowNew(Cpu *cpu) { rt::log("JNI: ThrowNew(%s, %s) ignored", class_of(cpu->r[1]).c_str(), rt::str(cpu->r[2])); cpu->r[0] = 0; }
void fn_zero(Cpu *cpu) { cpu->r[0] = 0; }
void fn_nop(Cpu *) {}
void fn_FatalError(Cpu *cpu) { rt::fatal("JNI FatalError: %s", rt::str(cpu->r[1])); }
void fn_PopLocalFrame(Cpu *cpu) { cpu->r[0] = cpu->r[1]; }
void fn_NewRef(Cpu *cpu) { cpu->r[0] = cpu->r[1]; }  // references are not tracked separately
void fn_DeleteLocalRef(Cpu *cpu) { (void)cpu; }      // locals live until explicit global delete
void fn_DeleteGlobalRef(Cpu *cpu) { (void)cpu; }
void fn_IsSameObject(Cpu *cpu) { cpu->r[0] = cpu->r[1] == cpu->r[2]; }
void fn_AllocObject(Cpu *cpu) { Obj *c = get(cpu->r[1]); cpu->r[0] = new_object(c ? c->cls : "?"); }
void fn_GetObjectClass(Cpu *cpu) { cpu->r[0] = find_class(class_of(cpu->r[1])); }
void fn_IsInstanceOf(Cpu *cpu) {
    Obj *o = get(cpu->r[1]), *c = get(cpu->r[2]);
    cpu->r[0] = o && c && (o->cls == c->cls || c->cls == "java/lang/Object");
}
void get_method_id(Cpu *cpu, bool is_static) {
    Obj *c = get(cpu->r[1]);
    std::string cls = c ? c->cls : "?";
    cpu->r[0] = member_id(g_methods, kMethodBase, {cls, rt::str(cpu->r[2]), rt::str(cpu->r[3]), is_static});
}
void fn_GetMethodID(Cpu *cpu) { get_method_id(cpu, false); }
void fn_GetStaticMethodID(Cpu *cpu) { get_method_id(cpu, true); }
void get_field_id(Cpu *cpu, bool is_static) {
    Obj *c = get(cpu->r[1]);
    std::string cls = c ? c->cls : "?";
    cpu->r[0] = member_id(g_fields, kFieldBase, {cls, rt::str(cpu->r[2]), rt::str(cpu->r[3]), is_static});
}
void fn_GetFieldID(Cpu *cpu) { get_field_id(cpu, false); }
void fn_GetStaticFieldID(Cpu *cpu) { get_field_id(cpu, true); }

// Call<Type>Method family. kind: 0 instance, 1 nonvirtual (extra clazz arg), 2 static.
template <int kind, ArgMode mode>
void fn_Call(Cpu *cpu) {
    u32 self = cpu->r[1];
    u32 mid = kind == 1 ? cpu->r[3] : cpu->r[2];
    u32 first = kind == 1 ? 4 : 3;
    const Member m = member(g_methods, kMethodBase, mid);
    char ret;
    std::string types = param_types(m.sig, &ret);
    Value v = invoke(m, self, read_args(cpu, types, mode, first));
    set_ret(cpu, ret, v);
}
template <ArgMode mode>
void fn_NewObject(Cpu *cpu) {
    Obj *c = get(cpu->r[1]);
    const Member m = member(g_methods, kMethodBase, cpu->r[2]);
    Handle obj = new_object(c ? c->cls : m.cls);
    char ret;
    std::string types = param_types(m.sig, &ret);
    invoke(m, obj, read_args(cpu, types, mode, 3));
    cpu->r[0] = obj;
}

// Get/Set<Type>Field, GetStatic/SetStatic<Type>Field
Value field_value(const Member &f, Handle obj) {
    if (f.is_static) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        auto it = g_static_fields.find(f.cls + "." + f.name);
        if (it != g_static_fields.end()) return it->second;
        warn_once("static field " + f.cls + "." + f.name + " " + f.sig);
        return Value{};
    }
    Obj *o = get(obj);
    if (o) {
        auto it = o->fields.find(f.name);
        if (it != o->fields.end()) return it->second;
    }
    warn_once("field " + f.cls + "." + f.name + " " + f.sig);
    return Value{};
}
template <char t>
void fn_GetField(Cpu *cpu) {
    const Member f = member(g_fields, kFieldBase, cpu->r[2]);
    set_ret(cpu, t, field_value(f, cpu->r[1]));
}
template <char t>
void fn_SetField(Cpu *cpu) {
    const Member f = member(g_fields, kFieldBase, cpu->r[2]);
    Args a(cpu, 3);
    Value v{};
    if (t == 'J' || t == 'D') v.raw = a.dword();
    else v.raw = a.word();
    if (f.is_static) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_static_fields[f.cls + "." + f.name] = v;
    } else if (Obj *o = get(cpu->r[1])) {
        o->fields[f.name] = v;
    }
}

// ---- strings ------------------------------------------------------------------------------------
std::u16string to_utf16(const std::string &s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = uint8_t(s[i]);
        int n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        if (n > 1) c &= 0x3f >> (n - 1);
        for (int k = 1; k < n && i + k < s.size(); k++) c = (c << 6) | (uint8_t(s[i + k]) & 0x3f);
        i += n;
        if (c >= 0x10000) { c -= 0x10000; out += char16_t(0xd800 + (c >> 10)); out += char16_t(0xdc00 + (c & 0x3ff)); }
        else out += char16_t(c);
    }
    return out;
}
// n UTF-16 units of guest memory at a (little-endian)
std::string from_utf16(u32 a, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = LD16(a + 2 * u32(i));
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n) { ++i; c = 0x10000 + ((c - 0xd800) << 10) + (LD16(a + 2 * u32(i)) - 0xdc00); }
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xc0 | c >> 6); out += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { out += char(0xe0 | c >> 12); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
        else { out += char(0xf0 | c >> 18); out += char(0x80 | ((c >> 12) & 63)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
    }
    return out;
}
void fn_NewString(Cpu *cpu) {
    cpu->r[0] = new_string(from_utf16(cpu->r[1], cpu->r[2]));
}
void fn_GetStringLength(Cpu *cpu) { cpu->r[0] = u32(to_utf16(string_of(cpu->r[1])).size()); }
void fn_GetStringChars(Cpu *cpu) {
    std::u16string s = to_utf16(string_of(cpu->r[1]));
    u32 buf = rt::gmalloc(u32(s.size() * 2 + 2));
    for (size_t i = 0; i <= s.size(); i++) ST16(buf + 2 * u32(i), s.c_str()[i]);
    if (cpu->r[2]) ST8(cpu->r[2], 1);
    cpu->r[0] = buf;
}
void fn_ReleaseChars(Cpu *cpu) { rt::gfree(cpu->r[2]); }
void fn_NewStringUTF(Cpu *cpu) { cpu->r[0] = cpu->r[1] ? new_string(rt::str(cpu->r[1])) : 0; }
void fn_GetStringUTFLength(Cpu *cpu) { cpu->r[0] = u32(string_of(cpu->r[1]).size()); }
void fn_GetStringUTFChars(Cpu *cpu) {
    if (cpu->r[2]) ST8(cpu->r[2], 1);
    cpu->r[0] = rt::gstrdup(string_of(cpu->r[1]).c_str());
}
void fn_GetStringRegion(Cpu *cpu) {
    std::u16string s = to_utf16(string_of(cpu->r[1]));
    u32 start = cpu->r[2], len = cpu->r[3], buf = reg(cpu, 4);
    for (u32 i = 0; i < len && start + i < s.size(); i++) ST16(buf + 2 * i, s[start + i]);
}
void fn_GetStringUTFRegion(Cpu *cpu) {
    std::string s = string_of(cpu->r[1]);
    u32 start = cpu->r[2], len = cpu->r[3], buf = reg(cpu, 4);
    std::string sub = start < s.size() ? s.substr(start, len) : "";
    std::memcpy(rt::ptr(buf), sub.c_str(), sub.size() + 1);
}

// ---- arrays ---------------------------------------------------------------------------------------
u32 elem_size(char t) { return t == 'Z' || t == 'B' ? 1 : t == 'C' || t == 'S' ? 2 : t == 'J' || t == 'D' ? 8 : 4; }
Handle new_array(char t, u32 len) {
    auto *o = new Obj{Obj::Array};
    o->cls = std::string("[") + t;
    o->elem = t;
    o->len = len;
    o->buf = rt::gcalloc(len ? len : 1, elem_size(t));
    return add(o);
}
template <char t> void fn_NewArray(Cpu *cpu) { cpu->r[0] = new_array(t, cpu->r[1]); }
void fn_GetArrayLength(Cpu *cpu) {
    Obj *o = get(cpu->r[1]);
    cpu->r[0] = !o ? 0 : o->elem == 'L' ? u32(o->elems.size()) : o->len;
}
void fn_NewObjectArray(Cpu *cpu) {
    auto *o = new Obj{Obj::Array};
    Obj *c = get(cpu->r[2]);
    o->cls = "[L" + (c ? c->cls : std::string("?")) + ";";
    o->elem = 'L';
    o->elems.assign(cpu->r[1], cpu->r[3]);
    cpu->r[0] = add(o);
}
void fn_GetObjectArrayElement(Cpu *cpu) {
    Obj *o = get(cpu->r[1]);
    cpu->r[0] = o && cpu->r[2] < o->elems.size() ? o->elems[cpu->r[2]] : 0;
}
void fn_SetObjectArrayElement(Cpu *cpu) {
    Obj *o = get(cpu->r[1]);
    if (o && cpu->r[2] < o->elems.size()) o->elems[cpu->r[2]] = cpu->r[3];
}
void fn_GetArrayElements(Cpu *cpu) {
    Obj *o = get(cpu->r[1]);
    if (cpu->r[2]) ST8(cpu->r[2], 0);
    cpu->r[0] = o ? o->buf : 0;
}
void fn_GetArrayRegion(Cpu *cpu) {
    Obj *o = get(cpu->r[1]);
    if (!o) return;
    u32 sz = elem_size(o->elem);
    std::memcpy(rt::ptr(reg(cpu, 4)), rt::ptr(o->buf + cpu->r[2] * sz), size_t(cpu->r[3]) * sz);
}
void fn_SetArrayRegion(Cpu *cpu) {
    Obj *o = get(cpu->r[1]);
    if (!o) return;
    u32 sz = elem_size(o->elem);
    std::memcpy(rt::ptr(o->buf + cpu->r[2] * sz), rt::ptr(reg(cpu, 4)), size_t(cpu->r[3]) * sz);
}

// ---- misc ---------------------------------------------------------------------------------------------
u32 g_env, g_vm;
void fn_RegisterNatives(Cpu *cpu) {
    Obj *c = get(cpu->r[1]);
    for (u32 i = 0; i < cpu->r[3]; i++) {
        u32 e = cpu->r[2] + 12 * i;
        rt::log("JNI: RegisterNatives %s.%s%s -> %08x", c ? c->cls.c_str() : "?", rt::str(LD32(e)), rt::str(LD32(e + 4)), LD32(e + 8));
    }
    cpu->r[0] = 0;
}
void fn_GetJavaVM(Cpu *cpu) { ST32(cpu->r[1], g_vm); cpu->r[0] = 0; }
void fn_NewDirectByteBuffer(Cpu *cpu) {
    auto *o = new Obj{Obj::Buffer};
    o->cls = "java/nio/ByteBuffer";
    o->buf = cpu->r[1];
    o->len = cpu->r[2];  // capacity is a jlong in r2:r3; buffers here are < 4 GiB
    cpu->r[0] = add(o);
}
void fn_GetDirectBufferAddress(Cpu *cpu) { Obj *o = get(cpu->r[1]); cpu->r[0] = o ? o->buf : 0; }
void fn_GetDirectBufferCapacity(Cpu *cpu) { Obj *o = get(cpu->r[1]); cpu->r[0] = o ? o->len : 0; cpu->r[1] = 0; }
void fn_GetObjectRefType(Cpu *cpu) { cpu->r[0] = 1; }  // JNILocalRefType

// JavaVM
void vm_GetEnv(Cpu *cpu) { ST32(cpu->r[1], g_env); cpu->r[0] = 0; }
void vm_Attach(Cpu *cpu) { if (cpu->r[1]) ST32(cpu->r[1], g_env); cpu->r[0] = 0; }

struct Slot { int index; std::string name; ImportFn fn; };

}  // namespace

// ---- public API ----------------------------------------------------------------------------------------
Handle new_string(const std::string &s) {
    auto *o = new Obj{Obj::String};
    o->cls = "java/lang/String";
    o->str = s;
    return add(o);
}
std::string string_of(Handle h) {
    Obj *o = get(h);
    return o ? o->str : std::string();
}
Handle find_class(const std::string &name) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_classes.find(name);
    if (it != g_classes.end()) return it->second;
    auto *o = new Obj{Obj::Class};
    o->cls = name;
    return g_classes[name] = add(o);
}
bool array_data(Handle h, uint32_t &guest_buf, uint32_t &length, char &elem) {
    Obj *o = get(h);
    if (!o || o->kind != Obj::Array || o->elem == 'L') return false;
    guest_buf = o->buf;
    length = o->len;
    elem = o->elem;
    return true;
}

Handle new_int_array(const std::vector<int32_t> &values) {
    Handle h = new_array('I', uint32_t(values.size()));
    Obj *o = get(h);
    for (size_t i = 0; i < values.size(); i++) ST32(o->buf + 4 * uint32_t(i), uint32_t(values[i]));
    return h;
}
Handle new_object(const std::string &cls) {
    auto *o = new Obj{Obj::Instance};
    o->cls = cls;
    return add(o);
}
void register_method(const std::string &key, Method m) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_impls[key] = std::move(m);
}
void register_static_field(const std::string &key, Value v) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_static_fields[key] = v;
}
uint32_t env() { return g_env; }
uint32_t vm() { return g_vm; }

uint32_t call_native(const char *export_name, const std::string &cls, std::initializer_list<uint32_t> args) {
    uint32_t fn = rt::export_address(export_name);
    if (!fn) rt::fatal("native method %s not exported", export_name);
    std::vector<uint32_t> all = {g_env, find_class(cls)};
    all.insert(all.end(), args.begin(), args.end());
    Cpu *cpu = rt::current_cpu();
    switch (all.size()) {
    case 2: return rt::call_nested(cpu, fn, {all[0], all[1]});
    case 3: return rt::call_nested(cpu, fn, {all[0], all[1], all[2]});
    case 4: return rt::call_nested(cpu, fn, {all[0], all[1], all[2], all[3]});
    case 5: return rt::call_nested(cpu, fn, {all[0], all[1], all[2], all[3], all[4]});
    case 6: return rt::call_nested(cpu, fn, {all[0], all[1], all[2], all[3], all[4], all[5]});
    case 7: return rt::call_nested(cpu, fn, {all[0], all[1], all[2], all[3], all[4], all[5], all[6]});
    }
    rt::fatal("call_native: too many arguments");
}

void register_java();  // java.cpp

void init() {
    if (g_env) return;
    using V = ArgMode;
    static const char *const kTypes = "LZBCSIJFDV";  // order of Call<Type>Method in the table
    std::vector<Slot> slots = {
        {4, "GetVersion", fn_GetVersion}, {6, "FindClass", fn_FindClass}, {7, "FromReflectedMethod", fn_zero},
        {8, "FromReflectedField", fn_zero}, {9, "ToReflectedMethod", fn_zero}, {10, "GetSuperclass", fn_GetSuperclass},
        {11, "IsAssignableFrom", fn_IsAssignableFrom}, {12, "ToReflectedField", fn_zero}, {13, "Throw", fn_Throw},
        {14, "ThrowNew", fn_ThrowNew}, {15, "ExceptionOccurred", fn_zero}, {16, "ExceptionDescribe", fn_nop},
        {17, "ExceptionClear", fn_nop}, {18, "FatalError", fn_FatalError}, {19, "PushLocalFrame", fn_zero},
        {20, "PopLocalFrame", fn_PopLocalFrame}, {21, "NewGlobalRef", fn_NewRef}, {22, "DeleteGlobalRef", fn_DeleteGlobalRef},
        {23, "DeleteLocalRef", fn_DeleteLocalRef}, {24, "IsSameObject", fn_IsSameObject}, {25, "NewLocalRef", fn_NewRef},
        {26, "EnsureLocalCapacity", fn_zero}, {27, "AllocObject", fn_AllocObject},
        {28, "NewObject", fn_NewObject<V::Variadic>}, {29, "NewObjectV", fn_NewObject<V::VaList>},
        {30, "NewObjectA", fn_NewObject<V::Array>}, {31, "GetObjectClass", fn_GetObjectClass},
        {32, "IsInstanceOf", fn_IsInstanceOf}, {33, "GetMethodID", fn_GetMethodID},
        {94, "GetFieldID", fn_GetFieldID}, {113, "GetStaticMethodID", fn_GetStaticMethodID},
        {144, "GetStaticFieldID", fn_GetStaticFieldID}, {163, "NewString", fn_NewString},
        {164, "GetStringLength", fn_GetStringLength}, {165, "GetStringChars", fn_GetStringChars},
        {166, "ReleaseStringChars", fn_ReleaseChars}, {167, "NewStringUTF", fn_NewStringUTF},
        {168, "GetStringUTFLength", fn_GetStringUTFLength}, {169, "GetStringUTFChars", fn_GetStringUTFChars},
        {170, "ReleaseStringUTFChars", fn_ReleaseChars}, {171, "GetArrayLength", fn_GetArrayLength},
        {172, "NewObjectArray", fn_NewObjectArray}, {173, "GetObjectArrayElement", fn_GetObjectArrayElement},
        {174, "SetObjectArrayElement", fn_SetObjectArrayElement},
        {175, "NewBooleanArray", fn_NewArray<'Z'>}, {176, "NewByteArray", fn_NewArray<'B'>},
        {177, "NewCharArray", fn_NewArray<'C'>}, {178, "NewShortArray", fn_NewArray<'S'>},
        {179, "NewIntArray", fn_NewArray<'I'>}, {180, "NewLongArray", fn_NewArray<'J'>},
        {181, "NewFloatArray", fn_NewArray<'F'>}, {182, "NewDoubleArray", fn_NewArray<'D'>},
        {215, "RegisterNatives", fn_RegisterNatives}, {216, "UnregisterNatives", fn_zero},
        {217, "MonitorEnter", fn_zero}, {218, "MonitorExit", fn_zero}, {219, "GetJavaVM", fn_GetJavaVM},
        {220, "GetStringRegion", fn_GetStringRegion}, {221, "GetStringUTFRegion", fn_GetStringUTFRegion},
        {222, "GetPrimitiveArrayCritical", fn_GetArrayElements}, {223, "ReleasePrimitiveArrayCritical", fn_nop},
        {224, "GetStringCritical", fn_GetStringChars}, {225, "ReleaseStringCritical", fn_ReleaseChars},
        {226, "NewWeakGlobalRef", fn_NewRef}, {227, "DeleteWeakGlobalRef", fn_nop}, {228, "ExceptionCheck", fn_zero},
        {229, "NewDirectByteBuffer", fn_NewDirectByteBuffer}, {230, "GetDirectBufferAddress", fn_GetDirectBufferAddress},
        {231, "GetDirectBufferCapacity", fn_GetDirectBufferCapacity}, {232, "GetObjectRefType", fn_GetObjectRefType},
    };
    // Call<Type>Method[V|A]: instance at 34, nonvirtual at 64, static at 114; 3 variants x 10 types each
    ImportFn instance[3] = {fn_Call<0, V::Variadic>, fn_Call<0, V::VaList>, fn_Call<0, V::Array>};
    ImportFn nonvirt[3] = {fn_Call<1, V::Variadic>, fn_Call<1, V::VaList>, fn_Call<1, V::Array>};
    ImportFn statik[3] = {fn_Call<2, V::Variadic>, fn_Call<2, V::VaList>, fn_Call<2, V::Array>};
    for (int t = 0; t < 10; t++)
        for (int k = 0; k < 3; k++) {
            const char *suffix[] = {"", "V", "A"};
            slots.push_back({34 + t * 3 + k, std::string("Call") + kTypes[t] + "Method" + suffix[k], instance[k]});
            slots.push_back({64 + t * 3 + k, std::string("CallNonvirtual") + kTypes[t] + "Method" + suffix[k], nonvirt[k]});
            slots.push_back({114 + t * 3 + k, std::string("CallStatic") + kTypes[t] + "Method" + suffix[k], statik[k]});
        }
    // fields: Get 95.., Set 104.., GetStatic 145.., SetStatic 154..   (order L Z B C S I J F D)
    ImportFn getters[9] = {fn_GetField<'L'>, fn_GetField<'Z'>, fn_GetField<'B'>, fn_GetField<'C'>, fn_GetField<'S'>,
                           fn_GetField<'I'>, fn_GetField<'J'>, fn_GetField<'F'>, fn_GetField<'D'>};
    ImportFn setters[9] = {fn_SetField<'L'>, fn_SetField<'Z'>, fn_SetField<'B'>, fn_SetField<'C'>, fn_SetField<'S'>,
                           fn_SetField<'I'>, fn_SetField<'J'>, fn_SetField<'F'>, fn_SetField<'D'>};
    for (int t = 0; t < 9; t++) {
        for (int base : {95, 104, 145, 154}) {
            slots.push_back({base + t, "Field_" + std::to_string(base) + "_" + kTypes[t],
                             base == 95 || base == 145 ? getters[t] : setters[t]});
        }
    }
    // primitive arrays: Get/Release elements 183/191, Get/Set region 199/207 (types Z B C S I J F D)
    for (int t = 0; t < 8; t++) {
        for (auto [base, fn] : {std::pair<int, ImportFn>{183, fn_GetArrayElements}, {191, fn_nop},
                                {199, fn_GetArrayRegion}, {207, fn_SetArrayRegion}}) {
            slots.push_back({base + t, "Array_" + std::to_string(base + t), fn});
        }
    }

    // Guest layout: [JavaVM* -> vm table][JNIEnv* -> env table][vm table: 8][env table: 233]
    u32 block = rt::gcalloc(1, 16 + 8 * 4 + 233 * 4);
    g_vm = block;
    g_env = block + 8;
    u32 vm_table = block + 16, env_table = vm_table + 8 * 4;
    ST32(g_vm, vm_table);
    ST32(g_env, env_table);
    auto install = [](u32 table, int index, const std::string &name, ImportFn fn) {
        rt::register_import(("JNI_" + name).c_str(), fn);
        ST32(table + 4 * index, rt::import_function_address(("JNI_" + name).c_str(), true));
    };
    // unimplemented JNIEnv slots trap with their index
    for (int i = 4; i < 233; i++)
        ST32(env_table + 4 * i, rt::import_function_address(("JNI_env_slot_" + std::to_string(i)).c_str(), true));
    for (auto &s : slots) install(env_table, s.index, s.name, s.fn);
    install(vm_table, 3, "DestroyJavaVM", fn_zero);
    install(vm_table, 4, "AttachCurrentThread", vm_Attach);
    install(vm_table, 5, "DetachCurrentThread", fn_zero);
    install(vm_table, 6, "GetEnv", vm_GetEnv);
    install(vm_table, 7, "AttachCurrentThreadAsDaemon", vm_Attach);
    register_java();
}

}  // namespace jni
