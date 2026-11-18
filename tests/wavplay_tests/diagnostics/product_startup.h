#pragma once

namespace wavplay_test {

// --test-product-startup runs the supplied product in a dedicated Windows logon
// session. Its real single-instance scope is shared by every copy in that session.
int run_product_startup_test(const wchar_t* executable);

} // namespace wavplay_test
