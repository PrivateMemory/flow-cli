#include <lua.h>
#include <lualib.h>
#include <luacode.h>
#include <string>
#include <Luau/CodeGen.h>
#include <Luau/Compiler.h>

// I copy pasted this from a source file of luau and tweaked it , i forgot which one it was lol

void SetupState(lua_State *L, bool sandbox_libs = false)
{
    luaL_openlibs(L);
    if (sandbox_libs)
        luaL_sandbox(L);
}

void RegisterFunctions(lua_State *L)
{
    // register ur custom funcs here
}

static Luau::CompileOptions copts()
{
    Luau::CompileOptions result = {};
    result.optimizationLevel = 0;
    result.debugLevel = 0;

    return result;
}

std::string runCode(lua_State *L, const std::string &code)
{
    const auto &bytecode = Luau::compile(code, copts(), {});
    int result = luau_load(L, "=main", bytecode.c_str(), bytecode.length(), 0);
    if (result != LUA_OK)
    {
        size_t len;
        const char *msg = lua_tolstring(L, -1, &len);

        std::string error(msg, len);
        lua_pop(L, 1);

        return error;
    }

    lua_State *T = lua_newthread(L);

    lua_pushvalue(L, -2);
    lua_remove(L, -3);
    lua_xmove(L, T, 1);

    int status = lua_resume(T, NULL, 0);

    if (status == 0)
    {
        int n = lua_gettop(T);

        if (n)
        {
            luaL_checkstack(T, LUA_MINSTACK, "too many results to print");
            lua_getglobal(T, "error");
            lua_insert(T, 1);
            lua_pcall(T, n, 0, 0);
        }

        lua_pop(L, 1); // pop T
        return std::string();
    }
    else
    {
        std::string error;

        lua_Debug ar;
        if (lua_getinfo(L, 0, "sln", &ar))
        {
            error += ar.short_src;
            error += ':';
            error += std::to_string(ar.currentline);
            error += ": ";
        }

        if (status == LUA_YIELD)
        {
            error += "thread yielded unexpectedly";
        }
        else if (const char *str = lua_tostring(T, -1))
        {
            error += str;
        }

        error += "\nstack backtrace:\n";
        error += lua_debugtrace(T);

        lua_pop(L, 1); // pop T
        return error;
    }
}

std::string executeScript(std::string script, bool sandbox_libs = false)
{
    for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true;

    std::unique_ptr<lua_State, void (*)(lua_State *)> globalState(luaL_newstate(), lua_close);
    lua_State *L = globalState.get();

    // setup state
    SetupState(L);

    // sandbox thread
    if (sandbox_libs)
        luaL_sandboxthread(L);

    // static string for caching result (prevents dangling ptr on function exit)
    static std::string result;
    RegisterFunctions(L);
    // run code + collect error
    result = runCode(L, script);

    return result.empty() ? "" : result;
}

#include <lstate.h>
#include <lvm.h>
#include <lgc.h>
#include <Flow/Flow.hpp>
#include <Luau/Bytecode.h>
#include <iostream>

/**
 * @brief Simulates VM_PROTECT
 *
 * @param L
 * @param ctx
 * @param f
 */
void ProtectedCall(lua_State* L, ExecutionContext ctx, std::function<void()> f)
{
    L->ci->savedpc = *ctx.pc;
    f();
    *ctx.base = L->base;
}

void vformatAppend(std::string &ret, const char *fmt, va_list args)
{
    va_list argscopy;
    va_copy(argscopy, args);
#ifdef _MSC_VER
    int actualSize = _vscprintf(fmt, argscopy);
#else
    int actualSize = vsnprintf(NULL, 0, fmt, argscopy);
#endif
    va_end(argscopy);

    if (actualSize <= 0)
        return;

    size_t sz = ret.size();
    ret.resize(sz + actualSize);
    vsnprintf(&ret[0] + sz, actualSize + 1, fmt, args);
}

void formatAppend(std::string &str, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vformatAppend(str, fmt, args);
    va_end(args);
}
#include <Luau/BytecodeBuilder.h>
#include <exception>

class FlowException : public std::exception
{
    std::string message;

public:
    FlowException(const std::string &message) : message(message) {}

    const char *what() const noexcept override
    {
        return message.c_str();
    }
};

#define OP_CHECK()              \
    if (this->op >= LOP__COUNT) \
        throw FlowException(__FUNCTION__##" Invalid opcode");


class Opcode
{
    std::uint32_t op;
    std::uint32_t flags;
    std::uint32_t aux;
public:

    union {
        struct {
            std::uint8_t a;
            std::uint8_t b;
            std::uint8_t c;
        } abc;

        struct {
            std::uint8_t a;
            std::int16_t d;
        } ad;

        struct {
            std::int32_t e;
        } e;
    } u;

    enum {
        OP_NONE = 0,
        OP_ABC = 1 << 0,
        OP_AD = 1 << 1,
        OP_E = 1 << 2,
        OP_AUX = 1 << 3,
        OP_MAX = 1 << 4,
    };
    Opcode(std::uint32_t op, std::uint32_t flags = 0, std::uint32_t aux = 0) : op(op), flags(flags), aux(aux) {
        if (getOpFlags(type()) & OP_ABC)
        {
            u.abc.a = LUAU_INSN_A(op);
            u.abc.b = LUAU_INSN_B(op);
            u.abc.c = LUAU_INSN_C(op);
        } else if (getOpFlags(type()) & OP_AD)
        {
            u.ad.a = LUAU_INSN_A(op);
            u.ad.d = LUAU_INSN_D(op);
        } else if (getOpFlags(type()) & OP_E)
        {
            u.e.e = LUAU_INSN_E(op);
        }
    }

    Opcode() : Opcode(LOP__COUNT, 0, 0) {}

    std::uint32_t getOp() const
    {
        return op;
    }

    void setAux(std::uint32_t aux)
    {
        if (this->getOpLen(type())!= 2)
            std::cout << "WARNING, THIS OPCODE DOESNT SUPPORT AUX " << op2str(type()) << std::endl;

        this->aux = aux;
    }

    static Opcode create(std::uint32_t op, std::uint32_t aux = 0)
    {
        if (LUAU_INSN_OP(op) >= LOP__COUNT)
            throw FlowException("Opcode::Create() Invalid opcode");
        
        return Opcode(op, getOpFlags(op2enum(op)), aux);
    }
    

    static LuauOpcode op2enum(std::uint32_t op)
    {
        // cap the op to prevent errors
        return std::min((LuauOpcode)LUAU_INSN_OP(op), LOP__COUNT);
    }

    static int getOpLen(LuauOpcode op)
    {
        if (op >= LOP__COUNT)
            return 0;

        switch (op)
        {
        case LOP_GETGLOBAL:
        case LOP_SETGLOBAL:
        case LOP_GETIMPORT:
        case LOP_GETTABLEKS:
        case LOP_SETTABLEKS:
        case LOP_NAMECALL:
        case LOP_JUMPIFEQ:
        case LOP_JUMPIFLE:
        case LOP_JUMPIFLT:
        case LOP_JUMPIFNOTEQ:
        case LOP_JUMPIFNOTLE:
        case LOP_JUMPIFNOTLT:
        case LOP_NEWTABLE:
        case LOP_SETLIST:
        case LOP_FORGLOOP:
        case LOP_LOADKX:
        case LOP_FASTCALL2:
        case LOP_FASTCALL2K:
        case LOP_FASTCALL3:
        case LOP_JUMPXEQKNIL:
        case LOP_JUMPXEQKB:
        case LOP_JUMPXEQKN:
        case LOP_JUMPXEQKS:
            return 2;

        default:
            return 1;
        }
    }

    static std::uint32_t getOpFlags(LuauOpcode op)
    {
        if (op >= LOP__COUNT)
            return 0;

        std::uint32_t flags = OP_NONE;
        switch (op)
        {
        case LOP_NOP:
        case LOP_BREAK:
        case LOP_LOADNIL:
        case LOP_LOADB:
        case LOP_MOVE:
        case LOP_GETGLOBAL:
        case LOP_SETGLOBAL:
        case LOP_GETUPVAL:
        case LOP_SETUPVAL:
        case LOP_CLOSEUPVALS:
        case LOP_GETTABLE:
        case LOP_SETTABLE:
        case LOP_GETTABLEKS:
        case LOP_SETTABLEKS:
        case LOP_GETTABLEN:
        case LOP_SETTABLEN:
        case LOP_NAMECALL:
        case LOP_CALL:
        case LOP_RETURN:
        case LOP_ADD:
        case LOP_SUB:
        case LOP_MUL:
        case LOP_DIV:
        case LOP_MOD:
        case LOP_POW:
        case LOP_ADDK:
        case LOP_SUBK:
        case LOP_MULK:
        case LOP_DIVK:
        case LOP_MODK:
        case LOP_POWK:
        case LOP_AND:
        case LOP_OR:
        case LOP_ANDK:
        case LOP_ORK:
        case LOP_CONCAT:
        case LOP_NOT:
        case LOP_MINUS:
        case LOP_LENGTH:
        case LOP_NEWTABLE:
        case LOP_SETLIST:
        case LOP_FORGPREP_INEXT:
        case LOP_FASTCALL3:
        case LOP_FORGPREP_NEXT:
        case LOP_NATIVECALL:
        case LOP_GETVARARGS:
        case LOP_PREPVARARGS:
        case LOP_LOADKX:
        case LOP_FASTCALL:
        case LOP_CAPTURE:
        case LOP_SUBRK:
        case LOP_DIVRK:
        case LOP_FASTCALL1:
        case LOP_FASTCALL2:
        case LOP_FASTCALL2K:
        case LOP_IDIV:
        case LOP_IDIVK:
            flags = OP_ABC;
            break;

        case LOP_LOADN:
        case LOP_LOADK:
        case LOP_GETIMPORT:
        case LOP_NEWCLOSURE:
        case LOP_JUMP:
        case LOP_JUMPBACK:
        case LOP_JUMPIF:
        case LOP_JUMPIFNOT:
        case LOP_JUMPIFEQ:
        case LOP_JUMPIFLE:
        case LOP_JUMPIFLT:
        case LOP_JUMPIFNOTEQ:
        case LOP_JUMPIFNOTLE:
        case LOP_JUMPIFNOTLT:
        case LOP_DUPTABLE:
        case LOP_FORNPREP:
        case LOP_FORNLOOP:
        case LOP_FORGLOOP:
        case LOP_DUPCLOSURE:
        case LOP_FORGPREP:
        case LOP_JUMPXEQKNIL:
        case LOP_JUMPXEQKB:
        case LOP_JUMPXEQKN:
        case LOP_JUMPXEQKS:
            flags = OP_AD;
            break;
        
        case LOP_JUMPX:
        case LOP_COVERAGE:
            flags = OP_E;
            break;

        default:
            break;
        }

        if (getOpLen(op) == 2)
            flags |= OP_AUX;

        return flags;
    }

    static const char* op2str(LuauOpcode op)
    {
        if (op >= LOP__COUNT)
            return "Unknown";

        switch (op)
        {
        case LOP_NOP:
            return "NOP";
        case LOP_BREAK:
            return "BREAK";
        case LOP_LOADNIL:
            return "LOADNIL";
        case LOP_LOADB:
            return "LOADB";
        case LOP_LOADN:
            return "LOADN";
        case LOP_LOADK:
            return "LOADK";
        case LOP_MOVE:
            return "MOVE";
        case LOP_GETGLOBAL:
            return "GETGLOBAL";
        case LOP_SETGLOBAL:
            return "SETGLOBAL";
        case LOP_GETUPVAL:
            return "GETUPVAL";
        case LOP_SETUPVAL:
            return "SETUPVAL";
        case LOP_CLOSEUPVALS:
            return "CLOSEUPVALS";
        case LOP_GETIMPORT:
            return "GETIMPORT";
        case LOP_GETTABLE:
            return "GETTABLE";
        case LOP_SETTABLE:
            return "SETTABLE";
        case LOP_GETTABLEKS:
            return "GETTABLEKS";
        case LOP_SETTABLEKS:
            return "SETTABLEKS";
        case LOP_GETTABLEN:
            return "GETTABLEN";
        case LOP_SETTABLEN:
            return "SETTABLEN";
        case LOP_NEWCLOSURE:
            return "NEWCLOSURE";
        case LOP_NAMECALL:
            return "NAMECALL";
        case LOP_CALL:
            return "CALL";
        case LOP_RETURN:
            return "RETURN";
        case LOP_JUMP:
            return "JUMP";
        case LOP_JUMPBACK:
            return "JUMPBACK";
        case LOP_JUMPIF:
            return "JUMPIF";
        case LOP_JUMPIFNOT:
            return "JUMPIFNOT";
        case LOP_JUMPIFEQ:
            return "JUMPIFEQ";
        case LOP_JUMPIFLE:
            return "JUMPIFLE";
        case LOP_JUMPIFLT:
            return "JUMPIFLT";
        case LOP_JUMPIFNOTEQ:
            return "JUMPIFNOTEQ";
        case LOP_JUMPIFNOTLE:
            return "JUMPIFNOTLE";
        case LOP_JUMPIFNOTLT:
            return "JUMPIFNOTLT";
        case LOP_ADD:
            return "ADD";
        case LOP_SUB:
            return "SUB";
        case LOP_MUL:
            return "MUL";
        case LOP_DIV:
            return "DIV";
        case LOP_MOD:
            return "MOD";
        case LOP_POW:
            return "POW";
        case LOP_ADDK:
            return "ADDK";
        case LOP_SUBK:
            return "SUBK";
        case LOP_MULK:
            return "MULK";
        case LOP_DIVK:
            return "DIVK";
        case LOP_MODK:
            return "MODK";
        case LOP_POWK:
            return "POWK";
        case LOP_AND:
            return "AND";
        case LOP_OR:
            return "OR";
        case LOP_ANDK:
            return "ANDK";
        case LOP_ORK:
            return "ORK";
        case LOP_CONCAT:
            return "CONCAT";
        case LOP_NOT:
            return "NOT";
        case LOP_MINUS:
            return "MINUS";
        case LOP_LENGTH:
            return "LENGTH";
        case LOP_NEWTABLE:
            return "NEWTABLE";
        case LOP_DUPTABLE:
            return "DUPTABLE";
        case LOP_SETLIST:
            return "SETLIST";
        case LOP_FORNPREP:
            return "FORNPREP";
        case LOP_FORNLOOP:
            return "FORNLOOP";
        case LOP_FORGLOOP:
            return "FORGLOOP";
        case LOP_FORGPREP_INEXT:
            return "FORGPREP_INEXT";
        case LOP_FASTCALL3:
            return "FASTCALL3";
        case LOP_FORGPREP_NEXT:
            return "FORGPREP_NEXT";
        case LOP_NATIVECALL:
            return "NATIVECALL";
        case LOP_GETVARARGS:
            return "GETVARARGS";
        case LOP_DUPCLOSURE:
            return "DUPCLOSURE";
        case LOP_PREPVARARGS:
            return "PREPVARARGS";
        case LOP_LOADKX:
            return "LOADKX";
        case LOP_JUMPX:
            return "JUMPX";
        case LOP_FASTCALL:
            return "FASTCALL";
        case LOP_COVERAGE:
            return "COVERAGE";
        case LOP_CAPTURE:
            return "CAPTURE";
        case LOP_SUBRK:
            return "SUBRK";
        case LOP_DIVRK:
            return "DIVRK";
        case LOP_FASTCALL1:
            return "FASTCALL1";
        case LOP_FASTCALL2:
            return "FASTCALL2";
        case LOP_FASTCALL2K:
            return "FASTCALL2K";
        case LOP_FORGPREP:
            return "FORGPREP";
        case LOP_JUMPXEQKNIL:
            return "JUMPXEQKNIL";
        case LOP_JUMPXEQKB:
            return "JUMPXEQKB";
        case LOP_JUMPXEQKN:
            return "JUMPXEQKN";
        case LOP_JUMPXEQKS:
            return "JUMPXEQKS";
        case LOP_IDIV:
            return "IDIV";
        case LOP_IDIVK:
            return "IDIVK";
        default:
            return "UNKNOWN";
        }
    }

    LuauOpcode type() const
    {
        return op2enum(this->op);
    }

    const char* name() const
    {
        return op2str(this->type());
    }

    std::uint32_t flag() {
        return this->flags;
    }

    std::uint32_t getAux() {
        return aux;
    }
};

#undef OP_CHECK

bool pre_op(lua_State* L, ExecutionContext ctx)
{
    std::uint32_t* pc = *ctx.pc;
    std::cout << Opcode::op2str(Opcode::op2enum(*pc)) << std::endl;
    std::uint32_t insn = *pc;
    if (Opcode::getOpLen(Opcode::op2enum(insn)) == 2)
    {
        std::uint32_t aux = *(pc + 1);
        //std::cout << aux << std::endl;
        lua_TValue* kv = &*ctx.k[aux];
        //std::cout << luaT_typenames[kv->tt] << std::endl;
        if (ttisstring(kv))
        {
            const char* str = svalue(kv);
            std::cout << "aux string : " << str << std::endl;
        }
    }
    /*std::uint32_t* pc = *ctx.pc;
    std::cout << std::hex << pc << " || " << *pc << std::endl;
    std::uint32_t insn = *pc++;
    std::cout << insn << std::endl;
    if (LUAU_INSN_OP(insn) >= LOP__COUNT)
        return true;
    std::cout << Opcode::op2str(Opcode::op2enum(insn)) << std::endl;
    if (Opcode::getOpLen(Opcode::op2enum(insn)) == 2) 
    {
        std::uint32_t aux = *pc++;
        std::cout << "sizek " << (*ctx.cl)->l.p->sizek << std::endl;
        std::cout << "aux : " << aux << std::endl;
        lua_TValue* kv = &*ctx.k[aux];
        std::cout << luaT_typenames[kv->tt] << std::endl;
        pc--;
    }
    pc--;*/
    //Opcode op = Opcode::getOpLen(Opcode::op2enum(*pc)) == 2 ? Opcode::create(*pc, *(pc+sizeof(std::uint32_t))) : Opcode::create(*pc);
    //std::cout << op.name() << std::endl;
/*
    if (op.type() == LOP_GETGLOBAL)
    {
        if (isLua(L->ci))
        {
            TValue* kv = ctx.k[op.getAux()];
            StkId ra = ctx.base[op.u.abc.a];
            if (ttisstring(kv))
            {
                TValue g;
                sethvalue(L, &g, clvalue(L->ci->func)->env);
                ProtectedCall(L, ctx, [ra,g,kv,L, ctx]() {
                    luaV_gettable(L, &g, kv, ra);
                });

                if (ttisfunction(ra))
                {
                    std::cout << "function found" << std::endl;
                }
            }
        }
    }
    
    return true;
    */
   return true;
}

/**
bool pre_op(lua_State *L, std::uint32_t* pc)
{
    if (LUAU_INSN_OP(*pc) >= LOP__COUNT)
        return true;
    auto op = Opcode::getOpLen(Opcode::op2enum(*pc)) == 2 ? Opcode::create(*pc, *(pc + sizeof(std::uint32_t))) : Opcode::create(*pc);
    std::cout << op.name() << std::endl;
    if (op.flag() & Opcode::OP_ABC)
    {
        printf("%i %i %i \n", op.u.abc.a, op.u.abc.b, op.u.abc.c);
    } else if (op.flag() & Opcode::OP_AD)
    {
        printf("%i %i \n", op.u.ad.a, op.u.ad.d);
    }

    if (op.type() == LOP_GETGLOBAL)
    {
        Closure* cl = clvalue(L->ci->func);
        if (isLua(L->ci))
        {
            Proto* p = cl->l.p;
            TValue* kv = &p->k[op.getAux()];
            if (ttisstring(kv))
            {
                TValue g;
                sethvalue(L, &g, cl->env);
            }

        }
    }

    return true;
}

void sort_op(lua_State *L, std::uint32_t* pc)
{
    
}
*/
int main()
{
    Flow::getInstance().set_pre_op(pre_op);
    //Flow::getInstance().set_post_op(sort_op);
    executeScript("print('hello world !')");
    return 0;
}