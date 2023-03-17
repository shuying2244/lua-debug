#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <symbolize/symbolize.h>

#include <algorithm>
#include <limits>

#include "rdebug_lua.h"
#include "rdebug_table.h"

int debug_pcall(lua_State* L, int nargs, int nresults, int errfunc);

lua_State* get_host(luadbg_State* L);

enum class VAR : uint8_t {
    FRAME_LOCAL,  // stack(frame, index)
    FRAME_FUNC,   // stack(frame).func
    UPVALUE,      // func[index]
    GLOBAL,       // _G
    REGISTRY,     // REGISTRY
    METATABLE,    // table.metatable
    USERVALUE,    // userdata.uservalue
    STACK,
    INDEX_KEY,
    INDEX_VAL,
    INDEX_INT,
    INDEX_STR,
};

struct value {
    VAR type;
    union {
        struct {
            uint16_t frame;
            int16_t n;
        } local;
        int index;
    };
};

// return record number of value
static int
sizeof_value(struct value* v) {
    switch (v->type) {
    case VAR::FRAME_LOCAL:
    case VAR::FRAME_FUNC:
    case VAR::GLOBAL:
    case VAR::REGISTRY:
    case VAR::STACK:
        return sizeof(struct value);
    case VAR::INDEX_STR:
        return sizeof_value((struct value*)((const char*)(v + 1) + v->index)) + sizeof(struct value) + v->index;
    case VAR::METATABLE:
        if (v->index != LUA_TTABLE && v->index != LUA_TUSERDATA) {
            return sizeof(struct value);
        }
        // go through
    case VAR::UPVALUE:
    case VAR::USERVALUE:
    case VAR::INDEX_KEY:
    case VAR::INDEX_VAL:
    case VAR::INDEX_INT:
        return sizeof_value(v + 1) + sizeof(struct value);
    }
    return 0;
}

static struct value*
create_value(luadbg_State* L, VAR type) {
    struct value* v = (struct value*)luadbg_newuserdata(L, sizeof(struct value));
    v->type         = type;
    return v;
}

static struct value*
create_value(luadbg_State* L, VAR type, int t, size_t extrasz = 0) {
    struct value* f = (struct value*)luadbg_touserdata(L, t);
    int sz          = sizeof_value(f);
    struct value* v = (struct value*)luadbg_newuserdata(L, sz + sizeof(struct value) + extrasz);
    v->type         = type;
    memcpy((char*)(v + 1) + extrasz, f, sz);
    return v;
}

// copy a value from -> to, return the lua type of copied or LUA_TNONE
static int
copy_toR(lua_State* from, luadbg_State* to) {
    int t = lua_type(from, -1);
    switch (t) {
    case LUA_TNIL:
        luadbg_pushnil(to);
        break;
    case LUA_TBOOLEAN:
        luadbg_pushboolean(to, lua_toboolean(from, -1));
        break;
    case LUA_TNUMBER:
#if LUA_VERSION_NUM >= 503 || defined(LUAJIT_VERSION)
        if (lua_isinteger(from, -1)) {
            luadbg_pushinteger(to, lua_tointeger(from, -1));
        }
        else {
            luadbg_pushnumber(to, lua_tonumber(from, -1));
        }
#else
        luadbg_pushnumber(to, lua_tonumber(from, -1));
#endif
        break;
    case LUA_TSTRING: {
        size_t sz;
        const char* str = lua_tolstring(from, -1, &sz);
        luadbg_pushlstring(to, str, sz);
        break;
    }
    case LUA_TLIGHTUSERDATA:
        luadbg_pushlightuserdata(to, lua_touserdata(from, -1));
        break;
    default:
        return LUA_TNONE;
    }
    return t;
}

static void
get_registry_value(luadbg_State* L, const char* name, int ref) {
    size_t len      = strlen(name);
    struct value* v = (struct value*)luadbg_newuserdata(L, 3 * sizeof(struct value) + len);
    v->type         = VAR::INDEX_INT;
    v->index        = ref;
    v++;

    v->type  = VAR::INDEX_STR;
    v->index = (int)len;
    v++;
    memcpy(v, name, len);
    v = (struct value*)((char*)v + len);

    v->type  = VAR::REGISTRY;
    v->index = 0;
}

static int
ref_value(lua_State* from, luadbg_State* to) {
    if (lua::getfield(from, LUA_REGISTRYINDEX, "__debugger_ref") == LUA_TNIL) {
        lua_pop(from, 1);
        lua_newtable(from);
        lua_pushvalue(from, -1);
        lua_setfield(from, LUA_REGISTRYINDEX, "__debugger_ref");
    }
    lua_pushvalue(from, -2);
    int ref = luaL_ref(from, -2);
    get_registry_value(to, "__debugger_ref", ref);
    lua_pop(from, 1);
    return ref;
}

void unref_value(lua_State* from, int ref) {
    if (ref >= 0) {
        if (lua::getfield(from, LUA_REGISTRYINDEX, "__debugger_ref") == LUA_TTABLE) {
            luaL_unref(from, -1, ref);
        }
        lua_pop(from, 1);
    }
}

int copy_value(lua_State* from, luadbg_State* to, bool ref) {
    if (copy_toR(from, to) == LUA_TNONE) {
        if (ref) {
            return ref_value(from, to);
        }
        else {
            luadbg_pushfstring(to, "%s: %p", lua_typename(from, lua_type(from, -1)), lua_topointer(from, -1));
            return LUA_NOREF;
        }
    }
    return LUA_NOREF;
}

// L top : value, uservalue
static int
eval_value_(lua_State* cL, struct value* v) {
    switch (v->type) {
    case VAR::FRAME_LOCAL: {
        lua_Debug ar;
        if (lua_getstack(cL, v->local.frame, &ar) == 0)
            break;
        const char* name = lua_getlocal(cL, &ar, v->local.n);
        if (name) {
            return lua_type(cL, -1);
        }
        break;
    }
    case VAR::FRAME_FUNC: {
        lua_Debug ar;
        if (lua_getstack(cL, v->index, &ar) == 0)
            break;
        if (lua_getinfo(cL, "f", &ar) == 0)
            break;
        return LUA_TFUNCTION;
    }
    case VAR::INDEX_INT: {
        int t = eval_value_(cL, v + 1);
        if (t == LUA_TNONE)
            break;
        if (t != LUA_TTABLE) {
            // only table can be index
            lua_pop(cL, 1);
            break;
        }
        lua_pushinteger(cL, (lua_Integer)v->index);
        lua_rawget(cL, -2);
        lua_replace(cL, -2);
        return lua_type(cL, -1);
    }
    case VAR::INDEX_STR: {
        int t = eval_value_(cL, (struct value*)((const char*)(v + 1) + v->index));
        if (t == LUA_TNONE)
            break;
        if (t != LUA_TTABLE) {
            // only table can be index
            lua_pop(cL, 1);
            break;
        }
        lua_pushlstring(cL, (const char*)(v + 1), (size_t)v->index);
        lua_rawget(cL, -2);
        lua_replace(cL, -2);
        return lua_type(cL, -1);
    }
    case VAR::INDEX_KEY:
    case VAR::INDEX_VAL: {
        int t = eval_value_(cL, v + 1);
        if (t == LUA_TNONE)
            break;
        if (t != LUA_TTABLE) {
            // only table can be index
            lua_pop(cL, 1);
            break;
        }
        bool ok = v->type == VAR::INDEX_KEY
                      ? luadebug::table::get_k(cL, -1, v->index)
                      : luadebug::table::get_v(cL, -1, v->index);
        if (!ok) {
            lua_pop(cL, 1);
            break;
        }
        lua_remove(cL, -2);
        return lua_type(cL, -1);
    }
    case VAR::UPVALUE: {
        int t = eval_value_(cL, v + 1);
        if (t == LUA_TNONE)
            break;
        if (t != LUA_TFUNCTION) {
            // only function has upvalue
            lua_pop(cL, 1);
            break;
        }
        if (lua_getupvalue(cL, -1, v->index)) {
            lua_replace(cL, -2);  // remove function
            return lua_type(cL, -1);
        }
        else {
            lua_pop(cL, 1);
            break;
        }
    }
    case VAR::GLOBAL:
#if LUA_VERSION_NUM == 501
        lua_pushvalue(cL, LUA_GLOBALSINDEX);
        return LUA_TTABLE;
#else
        return lua::rawgeti(cL, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
#endif
    case VAR::REGISTRY:
        lua_pushvalue(cL, LUA_REGISTRYINDEX);
        return LUA_TTABLE;
    case VAR::METATABLE:
        if (v->index != LUA_TTABLE && v->index != LUA_TUSERDATA) {
            switch (v->index) {
            case LUA_TNIL:
                lua_pushnil(cL);
                break;
            case LUA_TBOOLEAN:
                lua_pushboolean(cL, 0);
                break;
            case LUA_TNUMBER:
                lua_pushinteger(cL, 0);
                break;
            case LUA_TSTRING:
                lua_pushstring(cL, "");
                break;
            case LUA_TLIGHTUSERDATA:
                lua_pushlightuserdata(cL, NULL);
                break;
            default:
                return LUA_TNONE;
            }
        }
        else {
            int t = eval_value_(cL, v + 1);
            if (t == LUA_TNONE)
                break;
            if (t != LUA_TTABLE && t != LUA_TUSERDATA) {
                lua_pop(cL, 1);
                break;
            }
        }
        if (lua_getmetatable(cL, -1)) {
            lua_replace(cL, -2);
            return LUA_TTABLE;
        }
        else {
            lua_pop(cL, 1);
            lua_pushnil(cL);
            return LUA_TNIL;
        }
    case VAR::USERVALUE: {
        int t = eval_value_(cL, v + 1);
        if (t == LUA_TNONE)
            break;
        if (t != LUA_TUSERDATA) {
            lua_pop(cL, 1);
            break;
        }
        t = lua_getiuservalue(cL, -1, v->index);
        lua_replace(cL, -2);
        return t;
    }
    case VAR::STACK:
        lua_pushvalue(cL, v->index);
        return lua_type(cL, -1);
    }
    return LUA_TNONE;
}

static int
copy_fromR(luadbg_State* from, lua_State* to) {
    if (lua_checkstack(to, 1) == 0) {
        return luadbgL_error(from, "stack overflow");
    }
    int t = luadbg_type(from, -1);
    switch (t) {
    case LUA_TNIL:
        lua_pushnil(to);
        break;
    case LUA_TBOOLEAN:
        lua_pushboolean(to, luadbg_toboolean(from, -1));
        break;
    case LUA_TNUMBER:
        if (luadbg_isinteger(from, -1)) {
            lua_pushinteger(to, (lua_Integer)luadbg_tointeger(from, -1));
        }
        else {
            lua_pushnumber(to, luadbg_tonumber(from, -1));
        }
        break;
    case LUA_TSTRING: {
        size_t sz;
        const char* str = luadbg_tolstring(from, -1, &sz);
        lua_pushlstring(to, str, sz);
        break;
    }
    case LUA_TLIGHTUSERDATA:
        lua_pushlightuserdata(to, luadbg_touserdata(from, -1));
        break;
    case LUA_TUSERDATA: {
        if (lua_checkstack(to, 3) == 0) {
            return luadbgL_error(from, "stack overflow");
        }
        struct value* v = (struct value*)luadbg_touserdata(from, -1);
        return eval_value_(to, v);
    }
    default:
        return LUA_TNONE;
    }
    return t;
}

// assign cL top into ref object in L. pop cL.
// return 0 failed
static int
assign_value(struct value* v, lua_State* cL) {
    int top = lua_gettop(cL);
    switch (v->type) {
    case VAR::FRAME_LOCAL: {
        lua_Debug ar;
        if (lua_getstack(cL, v->local.frame, &ar) == 0) {
            break;
        }
        if (lua_setlocal(cL, &ar, v->local.n) != NULL) {
            return 1;
        }
        break;
    }
    case VAR::GLOBAL:
    case VAR::REGISTRY:
    case VAR::FRAME_FUNC:
    case VAR::STACK:
        // Can't assign frame func, etc.
        break;
    case VAR::INDEX_INT: {
        int t = eval_value_(cL, v + 1);
        if (t == LUA_TNONE)
            break;
        if (t != LUA_TTABLE) {
            // only table can be index
            break;
        }
        lua_pushinteger(cL, (lua_Integer)v->index);  // key, table, value, ...
        lua_pushvalue(cL, -3);                       // value, key, table, value, ...
        lua_rawset(cL, -3);                          // table, value, ...
        lua_pop(cL, 2);
        return 1;
    }
    case VAR::INDEX_STR: {
        int t = eval_value_(cL, (struct value*)((const char*)(v + 1) + v->index));
        if (t == LUA_TNONE)
            break;
        if (t != LUA_TTABLE) {
            // only table can be index
            break;
        }
        lua_pushlstring(cL, (const char*)(v + 1), (size_t)v->index);  // key, table, value, ...
        lua_pushvalue(cL, -3);                                        // value, key, table, value, ...
        lua_rawset(cL, -3);                                           // table, value, ...
        lua_pop(cL, 2);
        return 1;
    }
    case VAR::INDEX_KEY:
        break;
    case VAR::INDEX_VAL: {
        int t = eval_value_(cL, v + 1);
        if (t == LUA_TNONE)
            break;
        if (t != LUA_TTABLE) {
            break;
        }
        lua_insert(cL, -2);
        if (!luadebug::table::set_v(cL, -2, v->index)) {
            break;
        }
        lua_pop(cL, 1);
        return 1;
    }
    case VAR::UPVALUE: {
        int t = eval_value_(cL, v + 1);
        if (t == LUA_TNONE)
            break;
        if (t != LUA_TFUNCTION) {
            // only function has upvalue
            break;
        }
        // swap function and value
        lua_insert(cL, -2);
        if (lua_setupvalue(cL, -2, v->index) != NULL) {
            lua_pop(cL, 1);
            return 1;
        }
        break;
    }
    case VAR::METATABLE: {
        if (v->index != LUA_TTABLE && v->index != LUA_TUSERDATA) {
            switch (v->index) {
            case LUA_TNIL:
                lua_pushnil(cL);
                break;
            case LUA_TBOOLEAN:
                lua_pushboolean(cL, 0);
                break;
            case LUA_TNUMBER:
                lua_pushinteger(cL, 0);
                break;
            case LUA_TSTRING:
                lua_pushstring(cL, "");
                break;
            case LUA_TLIGHTUSERDATA:
                lua_pushlightuserdata(cL, NULL);
                break;
            default:
                // Invalid
                return 0;
            }
        }
        else {
            int t = eval_value_(cL, v + 1);
            if (t != LUA_TTABLE && t != LUA_TUSERDATA) {
                break;
            }
        }
        lua_insert(cL, -2);
        int metattype = lua_type(cL, -1);
        if (metattype != LUA_TNIL && metattype != LUA_TTABLE) {
            break;
        }
        lua_setmetatable(cL, -2);
        lua_pop(cL, 1);
        return 1;
    }
    case VAR::USERVALUE: {
        int t = eval_value_(cL, v + 1);
        if (t != LUA_TUSERDATA) {
            break;
        }
        lua_insert(cL, -2);
        lua_setiuservalue(cL, -2, v->index);
        lua_pop(cL, 1);
        return 1;
    }
    }
    lua_settop(cL, top - 1);
    return 0;
}

static const char*
get_frame_local(luadbg_State* L, lua_State* cL, uint16_t frame, int16_t n, int getref) {
    lua_Debug ar;
    if (lua_getstack(cL, frame, &ar) == 0) {
        return NULL;
    }
    if (lua_checkstack(cL, 1) == 0) {
        luadbgL_error(L, "stack overflow");
    }
    const char* name = lua_getlocal(cL, &ar, n);
    if (name == NULL)
        return NULL;
    if (!getref && copy_toR(cL, L) != LUA_TNONE) {
        lua_pop(cL, 1);
        return name;
    }
    lua_pop(cL, 1);
    struct value* v = create_value(L, VAR::FRAME_LOCAL);
    v->local.frame  = frame;
    v->local.n      = n;
    return name;
}

static void
get_frame_func(luadbg_State* L, int frame) {
    struct value* v = create_value(L, VAR::FRAME_FUNC);
    v->index        = frame;
}

// table key
static int
table_key(luadbg_State* L, lua_State* cL) {
    if (lua_checkstack(cL, 3) == 0) {
        return luadbgL_error(L, "stack overflow");
    }
    luadbg_insert(L, -2);  // L : key table
    if (copy_fromR(L, cL) != LUA_TTABLE) {
        lua_pop(cL, 1);    // pop table
        luadbg_pop(L, 2);  // pop k/t
        return 0;
    }
    luadbg_insert(L, -2);                  // L : table key
    if (copy_fromR(L, cL) == LUA_TNONE) {  // key
        lua_pop(cL, 1);                    // pop table
        luadbg_pop(L, 2);                  // pop k/t
        return 0;
    }
    return 1;
}

// table key
static void
new_index(luadbg_State* L) {
    struct value* v = create_value(L, VAR::INDEX_INT, -2);
    v->type         = VAR::INDEX_INT;
    v->index        = (int)luadbg_tointeger(L, -2);
}

// input cL : table key [value]
// input L :  table key
// output cL :
// output L : v(key or value)
static void
combine_index(luadbg_State* L, lua_State* cL, int getref) {
    if (!getref && copy_toR(cL, L) != LUA_TNONE) {
        lua_pop(cL, 2);
        // L : t, k, v
        luadbg_replace(L, -3);
        luadbg_pop(L, 1);
        return;
    }
    lua_pop(cL, 2);  // pop t v from cL
    // L : t, k
    new_index(L);
    // L : t, k, v
    luadbg_replace(L, -3);
    luadbg_pop(L, 1);
}

// table key
static void
new_field(luadbg_State* L) {
    size_t len      = 0;
    const char* str = luadbg_tolstring(L, -1, &len);
    struct value* v = create_value(L, VAR::INDEX_STR, -2, len);
    v->index        = (int)len;
    memcpy(v + 1, str, len);
}

// input cL : table key [value]
// input L :  table key
// output cL :
// output L : v(key or value)
static void
combine_field(luadbg_State* L, lua_State* cL, int getref) {
    if (!getref && copy_toR(cL, L) != LUA_TNONE) {
        lua_pop(cL, 2);
        // L : t, k, v
        luadbg_replace(L, -3);
        luadbg_pop(L, 1);
        return;
    }
    lua_pop(cL, 2);  // pop t v from cL
    // L : t, k
    new_field(L);
    // L : t, k, v
    luadbg_replace(L, -3);
    luadbg_pop(L, 1);
}

static const char*
get_upvalue(luadbg_State* L, lua_State* cL, int index, int getref) {
    if (luadbg_type(L, -1) != LUA_TUSERDATA) {
        luadbg_pop(L, 1);
        return NULL;
    }
    int t = copy_fromR(L, cL);
    if (t == LUA_TNONE) {
        luadbg_pop(L, 1);  // remove function object
        return NULL;
    }
    if (t != LUA_TFUNCTION) {
        luadbg_pop(L, 1);  // remove function object
        lua_pop(cL, 1);    // remove none function
        return NULL;
    }
    const char* name = lua_getupvalue(cL, -1, index);
    if (name == NULL) {
        luadbg_pop(L, 1);  // remove function object
        lua_pop(cL, 1);    // remove function
        return NULL;
    }

    if (!getref && copy_toR(cL, L) != LUA_TNONE) {
        luadbg_replace(L, -2);  // remove function object
        lua_pop(cL, 1);
        return name;
    }
    lua_pop(cL, 2);  // remove func / upvalue
    struct value* v = create_value(L, VAR::UPVALUE, -1);
    v->index        = index;
    luadbg_replace(L, -2);  // remove function object
    return name;
}

static int
get_registry(luadbg_State* L, VAR type) {
    switch (type) {
    case VAR::GLOBAL:
    case VAR::REGISTRY:
        break;
    default:
        return 0;
    }
    struct value* v = create_value(L, type);
    v->index        = 0;
    return 1;
}

static int
get_metatable(luadbg_State* L, lua_State* cL, int getref) {
    if (lua_checkstack(cL, 2) == 0)
        luadbgL_error(L, "stack overflow");
    int t = copy_fromR(L, cL);
    if (t == LUA_TNONE) {
        luadbg_pop(L, 1);
        return 0;
    }
    if (!getref) {
        if (lua_getmetatable(cL, -1) == 0) {
            luadbg_pop(L, 1);
            lua_pop(cL, 1);
            return 0;
        }
        lua_pop(cL, 2);
    }
    else {
        lua_pop(cL, 1);
    }
    if (t == LUA_TTABLE || t == LUA_TUSERDATA) {
        struct value* v = create_value(L, VAR::METATABLE, -1);
        v->type         = VAR::METATABLE;
        v->index        = t;
        luadbg_replace(L, -2);
        return 1;
    }
    else {
        luadbg_pop(L, 1);
        struct value* v = create_value(L, VAR::METATABLE);
        v->index        = t;
        return 1;
    }
}

static int
get_uservalue(luadbg_State* L, lua_State* cL, int index, int getref) {
    if (lua_checkstack(cL, 2) == 0)
        return luadbgL_error(L, "stack overflow");
    int t = copy_fromR(L, cL);
    if (t == LUA_TNONE) {
        luadbg_pop(L, 1);
        return 0;
    }

    if (t != LUA_TUSERDATA) {
        lua_pop(cL, 1);
        luadbg_pop(L, 1);
        return 0;
    }

    if (!getref) {
        if (lua_getiuservalue(cL, -1, index) == LUA_TNONE) {
            lua_pop(cL, 1);
            luadbg_pop(L, 1);
            return 0;
        }
        if (copy_toR(cL, L) != LUA_TNONE) {
            lua_pop(cL, 2);  // pop userdata / uservalue
            luadbg_replace(L, -2);
            return 1;
        }
    }

    // pop full userdata
    lua_pop(cL, 1);

    // L : value
    // cL : value uservalue
    struct value* v = create_value(L, VAR::USERVALUE, -1);
    v->index        = index;
    luadbg_replace(L, -2);
    return 1;
}

static void
combine_key(luadbg_State* L, lua_State* cL, int t, int index) {
    if (copy_toR(cL, L) != LUA_TNONE) {
        lua_pop(cL, 1);
        return;
    }
    lua_pop(cL, 1);
    struct value* v = create_value(L, VAR::INDEX_KEY, t);
    v->index        = index;
}

static void
combine_val(luadbg_State* L, lua_State* cL, int t, int index, int ref) {
    if (ref) {
        struct value* v = create_value(L, VAR::INDEX_VAL, t);
        v->index        = index;
        if (copy_toR(cL, L) == LUA_TNONE) {
            luadbg_pushvalue(L, -1);
        }
        lua_pop(cL, 1);
        return;
    }
    if (copy_toR(cL, L) == LUA_TNONE) {
        struct value* v = create_value(L, VAR::INDEX_VAL, t);
        v->index        = index;
    }
    lua_pop(cL, 1);
}

// frame, index
// return value, name
static int
client_getlocal(luadbg_State* L, int getref) {
    luadbg_Integer frame = luadbgL_checkinteger(L, 1);
    luadbg_Integer index = luadbgL_checkinteger(L, 2);
    if (frame < 0 || frame > (std::numeric_limits<uint16_t>::max)()) {
        return luadbgL_error(L, "frame must be `uint16_t`");
    }
    if (index == 0 || index > (std::numeric_limits<uint8_t>::max)() || -index > (std::numeric_limits<uint8_t>::max)()) {
        return luadbgL_error(L, "index must be `uint8_t`");
    }
    lua_State* cL    = get_host(L);
    const char* name = get_frame_local(L, cL, (uint16_t)frame, (int16_t)index, getref);
    if (name) {
        luadbg_pushstring(L, name);
        luadbg_insert(L, -2);
        return 2;
    }

    return 0;
}

static int
lclient_getlocal(luadbg_State* L) {
    return client_getlocal(L, 1);
}

static int
lclient_getlocalv(luadbg_State* L) {
    return client_getlocal(L, 0);
}

static int
client_index(luadbg_State* L, int getref) {
    lua_State* cL = get_host(L);
    if (luadbg_gettop(L) != 2) {
        return luadbgL_error(L, "need table key");
    }
    luadbg_Integer i = luadbgL_checkinteger(L, 2);
#ifdef LUAJIT_VERSION
    if (i < 0 || i > (std::numeric_limits<int>::max)()) {
#else
    if (i <= 0 || i > (std::numeric_limits<int>::max)()) {
#endif
        return luadbgL_error(L, "must be `unsigned int`");
    }
    if (table_key(L, cL) == 0)
        return 0;
    if (lua_type(cL, -2) != LUA_TTABLE) {
        lua_pop(cL, 2);
        return luadbgL_error(L, "#1 is not a table");
    }
    lua_rawget(cL, -2);  // cL : table value
    combine_index(L, cL, getref);
    return 1;
}

static int
lclient_index(luadbg_State* L) {
    return client_index(L, 1);
}

static int
lclient_indexv(luadbg_State* L) {
    return client_index(L, 0);
}

static int
client_field(luadbg_State* L, int getref) {
    lua_State* cL = get_host(L);
    if (luadbg_gettop(L) != 2) {
        return luadbgL_error(L, "need table key");
    }
    luadbgL_checktype(L, 2, LUA_TSTRING);
    if (table_key(L, cL) == 0)
        return 0;
    if (lua_type(cL, -2) != LUA_TTABLE) {
        lua_pop(cL, 2);
        return luadbgL_error(L, "#1 is not a table");
    }
    lua_rawget(cL, -2);  // cL : table value
    combine_field(L, cL, getref);
    return 1;
}

static int
lclient_field(luadbg_State* L) {
    return client_field(L, 1);
}

static int
lclient_fieldv(luadbg_State* L) {
    return client_field(L, 0);
}

static int
tablehash(luadbg_State* L, int ref) {
    lua_State* cL       = get_host(L);
    luadbg_Integer maxn = luadbgL_optinteger(L, 2, std::numeric_limits<unsigned int>::max());
    luadbg_settop(L, 1);
    if (lua_checkstack(cL, 4) == 0) {
        return luadbgL_error(L, "stack overflow");
    }
    if (copy_fromR(L, cL) != LUA_TTABLE) {
        lua_pop(cL, 1);
        return 0;
    }
    const void* t = lua_topointer(cL, -1);
    if (!t) {
        lua_pop(cL, 1);
        return 0;
    }
    luadbg_newtable(L);
    luadbg_Integer n   = 0;
    unsigned int hsize = luadebug::table::hash_size(t);
    unsigned int i     = 0;
    for (; i < hsize; ++i) {
        if (luadebug::table::get_kv(cL, t, i)) {
            if (--maxn < 0) {
                lua_pop(cL, 3);
                return 1;
            }
            combine_key(L, cL, 1, i);
            luadbg_rawseti(L, -2, ++n);
            combine_val(L, cL, 1, i, ref);
            if (ref) {
                luadbg_rawseti(L, -3, ++n);
            }
            luadbg_rawseti(L, -2, ++n);
        }
    }
    if (luadebug::table::get_zero(cL, t)) {
        if (--maxn < 0) {
            lua_pop(cL, 3);
            return 1;
        }
        combine_key(L, cL, 1, i);
        luadbg_rawseti(L, -2, ++n);
        combine_val(L, cL, 1, i, ref);
        if (ref) {
            luadbg_rawseti(L, -3, ++n);
        }
        luadbg_rawseti(L, -2, ++n);
    }
    lua_pop(cL, 1);
    return 1;
}

static int
lclient_tablehash(luadbg_State* L) {
    return tablehash(L, 1);
}

static int
lclient_tablehashv(luadbg_State* L) {
    return tablehash(L, 0);
}

static int
lclient_tablesize(luadbg_State* L) {
    lua_State* cL = get_host(L);
    if (copy_fromR(L, cL) != LUA_TTABLE) {
        lua_pop(cL, 1);
        return 0;
    }
    const void* t = lua_topointer(cL, -1);
    if (!t) {
        lua_pop(cL, 1);
        return 0;
    }
    luadbg_pushinteger(L, luadebug::table::array_size(t));
    luadbg_pushinteger(L, luadebug::table::hash_size(t) + (luadebug::table::has_zero(t) ? 1 : 0));
    lua_pop(cL, 1);
    return 2;
}

static int
lclient_tablekey(luadbg_State* L) {
    lua_State* cL    = get_host(L);
    unsigned int idx = (unsigned int)luadbgL_optinteger(L, 2, 0);
    luadbg_settop(L, 1);
    if (lua_checkstack(cL, 2) == 0) {
        return luadbgL_error(L, "stack overflow");
    }
    if (copy_fromR(L, cL) != LUA_TTABLE) {
        lua_pop(cL, 1);
        return 0;
    }
    const void* t = lua_topointer(cL, -1);
    if (!t) {
        lua_pop(cL, 1);
        return 0;
    }
    unsigned int hsize = luadebug::table::hash_size(t);
    for (unsigned int i = idx; i < hsize; ++i) {
        if (luadebug::table::get_k(cL, t, i)) {
            if (lua_type(cL, -1) == LUA_TSTRING) {
                size_t sz;
                const char* str = lua_tolstring(cL, -1, &sz);
                luadbg_pushlstring(L, str, sz);
                luadbg_pushinteger(L, i + 1);
                lua_pop(cL, 2);
                return 2;
            }
            lua_pop(cL, 1);
        }
    }
    lua_pop(cL, 1);
    return 0;
}

static int
lclient_udread(luadbg_State* L) {
    lua_State* cL         = get_host(L);
    luadbg_Integer offset = luadbgL_checkinteger(L, 2);
    luadbg_Integer count  = luadbgL_checkinteger(L, 3);
    luadbg_settop(L, 1);
    if (copy_fromR(L, cL) == LUA_TNONE) {
        return luadbgL_error(L, "Need userdata");
    }
    if (lua_type(cL, -1) != LUA_TUSERDATA) {
        lua_pop(cL, 1);
        return luadbgL_error(L, "Need userdata");
    }
    const char* memory = (const char*)lua_touserdata(cL, -1);
    size_t len         = (size_t)lua_rawlen(cL, -1);
    if (offset < 0 || (size_t)offset >= len || count <= 0) {
        lua_pop(cL, 1);
        return 0;
    }
    if ((size_t)(offset + count) > len) {
        count = (luadbg_Integer)len - offset;
    }
    luadbg_pushlstring(L, memory + offset, (size_t)count);
    lua_pop(cL, 1);
    return 1;
}

static int
lclient_udwrite(luadbg_State* L) {
    lua_State* cL         = get_host(L);
    luadbg_Integer offset = luadbgL_checkinteger(L, 2);
    size_t count          = 0;
    const char* data      = luadbgL_checklstring(L, 3, &count);
    int allowPartial      = luadbg_toboolean(L, 4);
    luadbg_settop(L, 1);
    if (copy_fromR(L, cL) == LUA_TNONE) {
        return luadbgL_error(L, "Need userdata");
    }
    if (lua_type(cL, -1) != LUA_TUSERDATA) {
        lua_pop(cL, 1);
        return luadbgL_error(L, "Need userdata");
    }
    const char* memory = (const char*)lua_touserdata(cL, -1);
    size_t len         = (size_t)lua_rawlen(cL, -1);
    if (allowPartial) {
        if (offset < 0 || (size_t)offset >= len) {
            lua_pop(cL, 1);
            luadbg_pushinteger(L, 0);
            return 1;
        }
        size_t bytesWritten = std::min(count, (size_t)(len - offset));
        memcpy((void*)(memory + offset), data, bytesWritten);
        lua_pop(cL, 1);
        luadbg_pushinteger(L, bytesWritten);
        return 1;
    }
    else {
        if (offset < 0 || (size_t)offset + count > len) {
            lua_pop(cL, 1);
            luadbg_pushboolean(L, 0);
            return 0;
        }
        memcpy((void*)(memory + offset), data, count);
        lua_pop(cL, 1);
        luadbg_pushboolean(L, 1);
        return 1;
    }
}

static int
lclient_value(luadbg_State* L) {
    lua_State* cL = get_host(L);
    luadbg_settop(L, 1);
    if (copy_fromR(L, cL) == LUA_TNONE) {
        luadbg_pop(L, 1);
        luadbg_pushnil(L);
        return 1;
    }
    luadbg_pop(L, 1);
    copy_value(cL, L, false);
    lua_pop(cL, 1);
    return 1;
}

// userdata ref
// any value
// ref = value
static int
lclient_assign(luadbg_State* L) {
    lua_State* cL = get_host(L);
    if (lua_checkstack(cL, 2) == 0)
        return luadbgL_error(L, "stack overflow");
    luadbg_settop(L, 2);
    if (copy_fromR(L, cL) == LUA_TNONE) {
        if (luadbg_type(L, 2) != LUA_TUSERDATA) {
            return luadbgL_error(L, "Invalid value type %s", luadbg_typename(L, luadbg_type(L, 2)));
        }
        lua_pushnil(cL);
    }
    if (lua_checkstack(cL, 3) == 0)
        return luadbgL_error(L, "stack overflow");
    luadbgL_checktype(L, 1, LUA_TUSERDATA);
    struct value* ref = (struct value*)luadbg_touserdata(L, 1);
    int r             = assign_value(ref, cL);
    luadbg_pushboolean(L, r);
    return 1;
}

static int
lclient_type(luadbg_State* L) {
    lua_State* cL = get_host(L);
    switch (luadbg_type(L, 1)) {
    case LUA_TNIL:
        luadbg_pushstring(L, "nil");
        return 1;
    case LUA_TBOOLEAN:
        luadbg_pushstring(L, "boolean");
        return 1;
    case LUA_TSTRING:
        luadbg_pushstring(L, "string");
        return 1;
    case LUA_TLIGHTUSERDATA:
        luadbg_pushstring(L, "lightuserdata");
        return 1;
    case LUA_TNUMBER:
#if LUA_VERSION_NUM >= 503 || defined(LUAJIT_VERSION)
        if (luadbg_isinteger(L, 1)) {
            luadbg_pushstring(L, "integer");
        }
        else {
            luadbg_pushstring(L, "float");
        }
#else
        luadbg_pushstring(L, "float");
#endif
        return 1;
    case LUA_TUSERDATA:
        break;
    default:
        luadbgL_error(L, "unexpected type: %s", luadbg_typename(L, luadbg_type(L, 1)));
        return 1;
    }
    if (lua_checkstack(cL, 3) == 0)
        return luadbgL_error(L, "stack overflow");
    luadbg_settop(L, 1);
    struct value* v = (struct value*)luadbg_touserdata(L, 1);
    int t           = eval_value_(cL, v);
    switch (t) {
    case LUA_TNONE:
        luadbg_pushstring(L, "unknown");
        return 1;
    case LUA_TFUNCTION:
        if (lua_iscfunction(cL, -1)) {
            luadbg_pushstring(L, "c function");
        }
        else {
            luadbg_pushstring(L, "function");
        }
        break;
    case LUA_TNUMBER:
#if LUA_VERSION_NUM >= 503 || defined(LUAJIT_VERSION)
        if (lua_isinteger(cL, -1)) {
            luadbg_pushstring(L, "integer");
        }
        else {
            luadbg_pushstring(L, "float");
        }
#else
        luadbg_pushstring(L, "float");
#endif
        break;
    case LUA_TLIGHTUSERDATA:
        luadbg_pushstring(L, "lightuserdata");
        break;
#ifdef LUAJIT_VERSION
    case LUA_TCDATA: {
        cTValue* o  = index2adr(cL, -1);
        GCcdata* cd = cdataV(o);
        if (cd->ctypeid == CTID_CTYPEID) {
            luadbg_pushstring(L, "ctype");
        }
        else {
            luadbg_pushstring(L, "cdata");
        }
    } break;
#endif
    default:
        luadbg_pushstring(L, lua_typename(cL, t));
        break;
    }
    lua_pop(cL, 1);
    return 1;
}

static int
client_getupvalue(luadbg_State* L, int getref) {
    int index = (int)luadbgL_checkinteger(L, 2);
    luadbg_settop(L, 1);
    lua_State* cL = get_host(L);

    const char* name = get_upvalue(L, cL, index, getref);
    if (name) {
        luadbg_pushstring(L, name);
        luadbg_insert(L, -2);
        return 2;
    }

    return 0;
}

static int
lclient_getupvalue(luadbg_State* L) {
    return client_getupvalue(L, 1);
}

static int
lclient_getupvaluev(luadbg_State* L) {
    return client_getupvalue(L, 0);
}

static int
client_getmetatable(luadbg_State* L, int getref) {
    luadbg_settop(L, 1);
    lua_State* cL = get_host(L);
    if (get_metatable(L, cL, getref)) {
        return 1;
    }
    return 0;
}

static int
lclient_getmetatable(luadbg_State* L) {
    return client_getmetatable(L, 1);
}

static int
lclient_getmetatablev(luadbg_State* L) {
    return client_getmetatable(L, 0);
}

static int
client_getuservalue(luadbg_State* L, int getref) {
    int n = (int)luadbgL_optinteger(L, 2, 1);
    luadbg_settop(L, 1);
    lua_State* cL = get_host(L);
    if (get_uservalue(L, cL, n, getref)) {
        luadbg_pushboolean(L, 1);
        return 2;
    }
    return 0;
}

static int
lclient_getuservalue(luadbg_State* L) {
    return client_getuservalue(L, 1);
}

static int
lclient_getuservaluev(luadbg_State* L) {
    return client_getuservalue(L, 0);
}

static int
lclient_getinfo(luadbg_State* L) {
    luadbg_settop(L, 3);
    size_t optlen       = 0;
    const char* options = luadbgL_checklstring(L, 2, &optlen);
    if (optlen > 7) {
        return luadbgL_error(L, "invalid option");
    }
    bool hasf = false;
    int frame = 0;
    int size  = 0;
#ifdef LUAJIT_VERSION
    bool hasSFlag = false;
#endif
    for (const char* what = options; *what; what++) {
        switch (*what) {
        case 'S':
            size += 5;
#ifdef LUAJIT_VERSION
            hasSFlag = true;
#endif
            break;
        case 'l':
            size += 1;
            break;
        case 'n':
            size += 2;
            break;
        case 'f':
            size += 1;
            hasf = true;
            break;
#if LUA_VERSION_NUM >= 502
        case 'u':
            size += 1;
            break;
        case 't':
            size += 1;
            break;
#endif
#if LUA_VERSION_NUM >= 504
        case 'r':
            size += 2;
            break;
#endif
        default:
            return luadbgL_error(L, "invalid option");
        }
    }
    if (luadbg_type(L, 3) != LUA_TTABLE) {
        luadbg_pop(L, 1);
        luadbg_createtable(L, 0, size);
    }

    lua_State* cL = get_host(L);
    lua_Debug ar;

    switch (luadbg_type(L, 1)) {
    case LUA_TNUMBER:
        frame = (int)luadbgL_checkinteger(L, 1);
        if (lua_getstack(cL, frame, &ar) == 0)
            return 0;
        if (lua_getinfo(cL, options, &ar) == 0)
            return 0;
        if (hasf) lua_pop(cL, 1);
        break;
    case LUA_TUSERDATA: {
        luadbg_pushvalue(L, 1);
        int t = copy_fromR(L, cL);
        if (t != LUA_TFUNCTION) {
            if (t != LUA_TNONE) {
                lua_pop(cL, 1);  // remove none function
            }
            return luadbgL_error(L, "Need a function ref, It's %s", luadbg_typename(L, t));
        }
        if (hasf) {
            return luadbgL_error(L, "invalid option");
        }
        luadbg_pop(L, 1);
        char what[8];
        what[0] = '>';
        strcpy(what + 1, options);
        if (lua_getinfo(cL, what, &ar) == 0)
            return 0;
        break;
    }
    default:
        return luadbgL_error(L, "Need stack level (integer) or function ref, It's %s", luadbg_typename(L, luadbg_type(L, 1)));
    }
#ifdef LUAJIT_VERSION
    if (hasSFlag && strcmp(ar.what, "main") == 0) {
        // carzy bug,luajit is real linedefined in main file,but in lua it's zero
        // maybe fix it is a new bug
        ar.lastlinedefined = 0;
    }
#endif

    for (const char* what = options; *what; what++) {
        switch (*what) {
        case 'S':
#if LUA_VERSION_NUM >= 504
            luadbg_pushlstring(L, ar.source, ar.srclen);
#else
            luadbg_pushstring(L, ar.source);
#endif
            luadbg_setfield(L, 3, "source");
            luadbg_pushstring(L, ar.short_src);
            luadbg_setfield(L, 3, "short_src");
            luadbg_pushinteger(L, ar.linedefined);
            luadbg_setfield(L, 3, "linedefined");
            luadbg_pushinteger(L, ar.lastlinedefined);
            luadbg_setfield(L, 3, "lastlinedefined");
            luadbg_pushstring(L, ar.what ? ar.what : "?");
            luadbg_setfield(L, 3, "what");
            break;
        case 'l':
            luadbg_pushinteger(L, ar.currentline);
            luadbg_setfield(L, 3, "currentline");
            break;
        case 'n':
            luadbg_pushstring(L, ar.name ? ar.name : "?");
            luadbg_setfield(L, 3, "name");
            if (ar.namewhat) {
                luadbg_pushstring(L, ar.namewhat);
            }
            else {
                luadbg_pushnil(L);
            }
            luadbg_setfield(L, 3, "namewhat");
            break;
        case 'f':
            get_frame_func(L, frame);
            luadbg_setfield(L, 3, "func");
            break;
#if LUA_VERSION_NUM >= 502
        case 'u':
            luadbg_pushinteger(L, ar.nparams);
            luadbg_setfield(L, 3, "nparams");
            break;
        case 't':
            luadbg_pushboolean(L, ar.istailcall ? 1 : 0);
            luadbg_setfield(L, 3, "istailcall");
            break;
#endif
#if LUA_VERSION_NUM >= 504
        case 'r':
            luadbg_pushinteger(L, ar.ftransfer);
            luadbg_setfield(L, 3, "ftransfer");
            luadbg_pushinteger(L, ar.ntransfer);
            luadbg_setfield(L, 3, "ntransfer");
            break;
#endif
        }
    }

    return 1;
}

static int
lclient_load(luadbg_State* L) {
    size_t len       = 0;
    const char* func = luadbgL_checklstring(L, 1, &len);
    lua_State* cL    = get_host(L);
    if (luaL_loadbuffer(cL, func, len, "=")) {
        luadbg_pushnil(L);
        luadbg_pushstring(L, lua_tostring(cL, -1));
        lua_pop(cL, 2);
        return 2;
    }
    ref_value(cL, L);
    lua_pop(cL, 1);
    return 1;
}

static int
eval_copy_args(luadbg_State* from, lua_State* to) {
    int t = copy_fromR(from, to);
    if (t == LUA_TNONE) {
        if (luadbg_type(from, -1) == LUA_TTABLE) {
            if (lua_checkstack(to, 3) == 0) {
                return luadbgL_error(from, "stack overflow");
            }
            lua_newtable(to);
            luadbg_pushnil(from);
            while (luadbg_next(from, -2)) {
                copy_fromR(from, to);
                luadbg_pop(from, 1);
                copy_fromR(from, to);
                lua_insert(to, -2);
                lua_rawset(to, -3);
            }
            return LUA_TTABLE;
        }
        else {
            lua_pushnil(to);
        }
    }
    return t;
}

static int
lclient_eval(luadbg_State* L) {
    lua_State* cL = get_host(L);
    int nargs     = luadbg_gettop(L);
    if (lua_checkstack(cL, nargs) == 0) {
        return luadbgL_error(L, "stack overflow");
    }
    for (int i = 1; i <= nargs; ++i) {
        luadbg_pushvalue(L, i);
        int t = eval_copy_args(L, cL);
        luadbg_pop(L, 1);
        if (i == 1 && t != LUA_TFUNCTION) {
            lua_pop(cL, 1);
            return luadbgL_error(L, "need function");
        }
    }
    if (debug_pcall(cL, nargs - 1, 1, 0)) {
        luadbg_pushboolean(L, 0);
        luadbg_pushstring(L, lua_tostring(cL, -1));
        lua_pop(cL, 1);
        return 2;
    }
    luadbg_pushboolean(L, 1);
    copy_value(cL, L, false);
    lua_pop(cL, 1);
    return 2;
}

static int
addwatch(lua_State* cL, int idx) {
    lua_pushvalue(cL, idx);
    if (lua::getfield(cL, LUA_REGISTRYINDEX, "__debugger_watch") == LUA_TNIL) {
        lua_pop(cL, 1);
        lua_newtable(cL);
        lua_pushvalue(cL, -1);
        lua_setfield(cL, LUA_REGISTRYINDEX, "__debugger_watch");
    }
    lua_insert(cL, -2);
    int ref = luaL_ref(cL, -2);
    lua_pop(cL, 1);
    return ref;
}

static int
lclient_watch(luadbg_State* L) {
    lua_State* cL = get_host(L);
    int n         = lua_gettop(cL);
    int nargs     = luadbg_gettop(L);
    if (lua_checkstack(cL, nargs) == 0) {
        return luadbgL_error(L, "stack overflow");
    }
    for (int i = 1; i <= nargs; ++i) {
        luadbg_pushvalue(L, i);
        int t = eval_copy_args(L, cL);
        luadbg_pop(L, 1);
        if (i == 1 && t != LUA_TFUNCTION) {
            lua_pop(cL, 1);
            return luadbgL_error(L, "need function");
        }
    }
    if (debug_pcall(cL, nargs - 1, LUA_MULTRET, 0)) {
        luadbg_pushboolean(L, 0);
        luadbg_pushstring(L, lua_tostring(cL, -1));
        lua_pop(cL, 1);
        return 2;
    }
    if (lua_checkstack(cL, 3) == 0) {
        return luadbgL_error(L, "stack overflow");
    }
    luadbg_pushboolean(L, 1);
    int rets = lua_gettop(cL) - n;
    luadbgL_checkstack(L, rets, NULL);
    for (int i = 0; i < rets; ++i) {
        get_registry_value(L, "__debugger_watch", addwatch(cL, i - rets));
    }
    lua_settop(cL, n);
    return 1 + rets;
}

static int
lclient_cleanwatch(luadbg_State* L) {
    lua_State* cL = get_host(L);
    lua_pushnil(cL);
    lua_setfield(cL, LUA_REGISTRYINDEX, "__debugger_watch");
    return 0;
}

static const char* costatus(lua_State* L, lua_State* co) {
    if (L == co) return "running";
    switch (lua_status(co)) {
    case LUA_YIELD:
        return "suspended";
    case LUA_OK: {
        lua_Debug ar;
        if (lua_getstack(co, 0, &ar)) return "normal";
        if (lua_gettop(co) == 0) return "dead";
        return "suspended";
    }
    default:
        return "dead";
    }
}

static int
lclient_costatus(luadbg_State* L) {
    lua_State* cL = get_host(L);
    if (copy_fromR(L, cL) == LUA_TNONE) {
        luadbg_pushstring(L, "invalid");
        return 1;
    }
    if (lua_type(cL, -1) != LUA_TTHREAD) {
        lua_pop(cL, 1);
        luadbg_pushstring(L, "invalid");
        return 1;
    }
    const char* s = costatus(cL, lua_tothread(cL, -1));
    lua_pop(cL, 1);
    luadbg_pushstring(L, s);
    return 1;
}

static int
lclient_gccount(luadbg_State* L) {
    lua_State* cL = get_host(L);
    int k         = lua_gc(cL, LUA_GCCOUNT, 0);
    int b         = lua_gc(cL, LUA_GCCOUNTB, 0);
    size_t m      = ((size_t)k << 10) & (size_t)b;
    luadbg_pushinteger(L, (luadbg_Integer)m);
    return 1;
}

static int lclient_cfunctioninfo(luadbg_State* L) {
    lua_State* cL = get_host(L);
    if (copy_fromR(L, cL) == LUA_TNONE) {
        luadbg_pushnil(L);
        return 1;
    }
#ifdef LUAJIT_VERSION
    cTValue* o = index2adr(cL, -1);
    void* cfn  = nullptr;
    if (tvisfunc(o)) {
        GCfunc* fn = funcV(o);
        cfn        = (void*)(isluafunc(fn) ? NULL : fn->c.f);
    }
    else if (tviscdata(o)) {
        GCcdata* cd  = cdataV(o);
        CTState* cts = ctype_cts(cL);
        if (cd->ctypeid != CTID_CTYPEID) {
            cfn = cdataptr(cd);
            if (cfn) {
                CType* ct = ctype_get(cts, cd->ctypeid);
                if (ctype_isref(ct->info) || ctype_isptr(ct->info)) {
                    cfn = cdata_getptr(cfn, ct->size);
                    ct  = ctype_rawchild(cts, ct);
                }
                if (!ctype_isfunc(ct->info)) {
                    cfn = nullptr;
                }
                else if (cfn) {
                    cfn = cdata_getptr(cfn, ct->size);
                }
            }
        }
    }
#else
    if (lua_type(cL, -1) != LUA_TFUNCTION) {
        lua_pop(cL, 1);
        luadbg_pushnil(L);
        return 1;
    }
    lua_CFunction cfn = lua_tocfunction(cL, -1);
#endif

    lua_pop(cL, 1);

    auto info = luadebug::symbolize((void*)cfn);
    if (!info.has_value()) {
        luadbg_pushnil(L);
        return 1;
    }
    luadbg_pushlstring(L, info->c_str(), info->size());
    return 1;
}

int init_visitor(luadbg_State* L) {
    luadbgL_Reg l[] = {
        { "getlocal", lclient_getlocal },
        { "getlocalv", lclient_getlocalv },
        { "getupvalue", lclient_getupvalue },
        { "getupvaluev", lclient_getupvaluev },
        { "getmetatable", lclient_getmetatable },
        { "getmetatablev", lclient_getmetatablev },
        { "getuservalue", lclient_getuservalue },
        { "getuservaluev", lclient_getuservaluev },
        { "index", lclient_index },
        { "indexv", lclient_indexv },
        { "field", lclient_field },
        { "fieldv", lclient_fieldv },
        { "tablehash", lclient_tablehash },
        { "tablehashv", lclient_tablehashv },
        { "tablesize", lclient_tablesize },
        { "tablekey", lclient_tablekey },
        { "udread", lclient_udread },
        { "udwrite", lclient_udwrite },
        { "value", lclient_value },
        { "assign", lclient_assign },
        { "type", lclient_type },
        { "getinfo", lclient_getinfo },
        { "load", lclient_load },
        { "eval", lclient_eval },
        { "watch", lclient_watch },
        { "cleanwatch", lclient_cleanwatch },
        { "costatus", lclient_costatus },
        { "gccount", lclient_gccount },
        { "cfunctioninfo", lclient_cfunctioninfo },
        { NULL, NULL },
    };
    luadbg_newtable(L);
    luadbgL_setfuncs(L, l, 0);
    get_registry(L, VAR::GLOBAL);
    luadbg_setfield(L, -2, "_G");
    get_registry(L, VAR::REGISTRY);
    luadbg_setfield(L, -2, "_REGISTRY");
    return 1;
}

LUADEBUG_FUNC
int luaopen_luadebug_visitor(luadbg_State* L) {
    get_host(L);
    return init_visitor(L);
}
