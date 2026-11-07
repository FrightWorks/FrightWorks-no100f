#include "runtime_product.h"

namespace RuntimeProduct {

const Descriptor& Active() noexcept {
    static constexpr Descriptor descriptor{
        Kind::ModProduct,
        "Mod",
    };
    return descriptor;
}

} // namespace RuntimeProduct
