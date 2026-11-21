#ifndef MOCHA_IL2CPP_API_H
#define MOCHA_IL2CPP_API_H

#include <mocha/mocha_api.h>

namespace mocha {

bool resolve_il2cpp_api(void* library_handle);
const MochaIl2CppApi* il2cpp_api();
size_t il2cpp_missing_symbols();

} // namespace mocha

#endif
