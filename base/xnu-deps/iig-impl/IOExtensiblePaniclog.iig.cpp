// Linux bring-up: forward inherited RPCs; class-specific RPCs remain unsupported.
// Local kernel IOExtensiblePaniclog methods are implemented in IOExtensiblePaniclog.cpp.
#include <IOKit/IOExtensiblePaniclog.h>
kern_return_t IOExtensiblePaniclog::Dispatch(const IORPC rpc)
{
 return OSObject::Dispatch(rpc);
}
kern_return_t IOExtensiblePaniclog::MetaClass::Dispatch(const IORPC rpc)
{
 return OSMetaClass::Dispatch(rpc);
}
