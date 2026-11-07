#include "runtime_product.h"

namespace RuntimeProduct {

const Descriptor& Active() noexcept {
    static constexpr Descriptor descriptor{
        Kind::ModProduct,
        "Retro Rewind",
    };
    return descriptor;
}

} // namespace RuntimeProduct
