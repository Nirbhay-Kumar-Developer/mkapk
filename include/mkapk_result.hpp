#ifndef MKAPK_RESULT_HPP
#define MKAPK_RESULT_HPP

#include <variant>
#include <string>
#include <utility>

// Primary template for returning a value or an error
template <typename T, typename E = std::string>
class Result {
private:
    std::variant<T, E> value;
    bool is_success = false;

    // Disambiguated private constructors using in-place index tags
    Result(std::in_place_index_t<0>, T val)
        : value(std::in_place_index<0>, std::move(val)), is_success(true) {}

    Result(std::in_place_index_t<1>, E err)
        : value(std::in_place_index<1>, std::move(err)), is_success(false) {}

public:
    static Result success(T val) {
        return Result(std::in_place_index<0>, std::move(val));
    }

    static Result error(E err) {
        return Result(std::in_place_index<1>, std::move(err));
    }

    bool is_ok() const { return is_success; }
    bool is_err() const { return !is_success; }

    const T& get_value() const { return std::get<0>(value); }
    T& get_value() { return std::get<0>(value); }

    const E& get_error() const { return std::get<1>(value); }
    E& get_error() { return std::get<1>(value); }
};

// Specialization for void returns (operations that just succeed or fail)
template <typename E>
class Result<void, E> {
private:
    std::variant<std::monostate, E> value;
    bool is_success = false;

    Result(std::in_place_index_t<0>)
        : value(std::in_place_index<0>, std::monostate{}), is_success(true) {}

    Result(std::in_place_index_t<1>, E err)
        : value(std::in_place_index<1>, std::move(err)), is_success(false) {}

public:
    static Result success() {
        return Result(std::in_place_index<0>);
    }

    static Result error(E err) {
        return Result(std::in_place_index<1>, std::move(err));
    }

    bool is_ok() const { return is_success; }
    bool is_err() const { return !is_success; }

    const E& get_error() const { return std::get<1>(value); }
    E& get_error() { return std::get<1>(value); }
};

#endif // MKAPK_RESULT_HPP
