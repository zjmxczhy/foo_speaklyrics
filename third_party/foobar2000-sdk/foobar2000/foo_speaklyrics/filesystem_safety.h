#pragma once

#include <filesystem>
#include <new>
#include <system_error>
#include <utility>

namespace speaklyrics_filesystem {

struct boolean_result {
    bool value = false;
    std::error_code error;
};

namespace detail {

inline std::error_code normalize_error(std::error_code error) noexcept {
    return error ? error : std::make_error_code(std::errc::io_error);
}

template <typename Operation>
inline boolean_result query_boolean(Operation&& operation) noexcept {
    try {
        std::error_code error;
        const bool value = operation(error);
        return { value, error };
    } catch (const std::filesystem::filesystem_error& exception) {
        return { false, normalize_error(exception.code()) };
    } catch (const std::bad_alloc&) {
        return { false, std::make_error_code(std::errc::not_enough_memory) };
    } catch (...) {
        return { false, std::make_error_code(std::errc::io_error) };
    }
}

inline std::error_code exception_error(
    const std::filesystem::filesystem_error& exception) noexcept {
    return normalize_error(exception.code());
}

inline std::error_code allocation_error() noexcept {
    return std::make_error_code(std::errc::not_enough_memory);
}

inline std::error_code unknown_error() noexcept {
    return std::make_error_code(std::errc::io_error);
}

} // namespace detail

inline boolean_result exists(const std::filesystem::path& path) noexcept {
    return detail::query_boolean([&](std::error_code& error) {
        return std::filesystem::exists(path, error);
    });
}

inline boolean_result is_directory(const std::filesystem::path& path) noexcept {
    return detail::query_boolean([&](std::error_code& error) {
        return std::filesystem::is_directory(path, error);
    });
}

inline boolean_result is_regular_file(const std::filesystem::path& path) noexcept {
    return detail::query_boolean([&](std::error_code& error) {
        return std::filesystem::is_regular_file(path, error);
    });
}

inline std::error_code open_directory(const std::filesystem::path& path,
    std::filesystem::directory_iterator& iterator) noexcept {
    try {
        std::error_code error;
        std::filesystem::directory_iterator candidate(path, error);
        if (error) return error;
        iterator = std::move(candidate);
        return {};
    } catch (const std::filesystem::filesystem_error& exception) {
        return detail::exception_error(exception);
    } catch (const std::bad_alloc&) {
        return detail::allocation_error();
    } catch (...) {
        return detail::unknown_error();
    }
}

inline std::error_code increment_directory(
    std::filesystem::directory_iterator& iterator) noexcept {
    try {
        std::error_code error;
        iterator.increment(error);
        return error;
    } catch (const std::filesystem::filesystem_error& exception) {
        return detail::exception_error(exception);
    } catch (const std::bad_alloc&) {
        return detail::allocation_error();
    } catch (...) {
        return detail::unknown_error();
    }
}

} // namespace speaklyrics_filesystem
