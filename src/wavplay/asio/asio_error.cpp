#include "wavplay/asio/asio_error.h"

#include <exception>
#include <stdexcept>
#include <string>

namespace wavplay::asio {

std::string error_message(ASIOError error, const char* operation)
{
    // Integer formatting only: no std::format or floating-point text.
    std::string message(operation);
    message += ": ";
    message += error_name(error);
    message += " (";
    message += std::to_string(error);
    message += ')';
    return message;
}

void throw_if_error(ASIOError error, const char* operation)
{
    if (error != ASE_OK) {
        // Cleanup can fail while handling an earlier error. Keep the original
        // exception (and its type) so diagnosis can walk the whole failure chain.
        // Do not attach an empty nested_exception to an ordinary leaf error.
        if (std::current_exception()) {
            std::throw_with_nested(std::runtime_error(error_message(error, operation)));
        }
        throw std::runtime_error(error_message(error, operation));
    }
}

} // namespace wavplay::asio
