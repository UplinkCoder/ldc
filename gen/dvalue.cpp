#include "gen/llvm.h"

#include "gen/tollvm.h"
#include "gen/irstate.h"
#include "gen/logger.h"
#include "gen/dvalue.h"
#include "gen/llvmhelpers.h"

#include "declaration.h"


/////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////

DVarValue::DVarValue(Type* t, VarDeclaration* vd, LLValue* llvmValue)
: DValue(t), var(vd), val(llvmValue)
{
    assert(isaPointer(llvmValue));
}

DVarValue::DVarValue(Type* t, LLValue* llvmValue)
: DValue(t), var(0), val(llvmValue)
{
    assert(isaPointer(llvmValue));
}

LLValue* DVarValue::getLVal()
{
    assert(val);
    return val;
}

LLValue* DVarValue::getRVal()
{
    assert(val);
    Type* bt = type->toBasetype();
    if (DtoIsPassedByRef(bt))
        return val;
    return DtoLoad(val);
}

/////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////

LLValue* DSliceValue::getRVal()
{
    assert(len);
    assert(ptr);
    return DtoAggrPair(len, ptr);
}

/////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////

DFuncValue::DFuncValue(FuncDeclaration* fd, LLValue* v, LLValue* vt)
: DValue(fd->type), func(fd), val(v), vthis(vt)
{}

LLValue* DFuncValue::getRVal()
{
    assert(val);
    return val;
}

/////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////

LLValue* DConstValue::getRVal()
{
    assert(c);
    return c;
}

/////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////
