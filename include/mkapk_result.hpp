#ifndef MKAPK_RESULT_HPP
#define MKAPK_RESULT_HPP

#include <variant>
#include <string>

// Primary template for returning a value or an error
template <typename T, typename E = std::string>
class Result {
private:
    std::variant<T, E> value;
    bool is_success;

public:
    static Result success(T val) {
        Result res;
        res.value = std::move(val);
        res.is_success = true;
        return res;
    }

    static Result error(E err) {
        Result res;
        res.value = std::move(err);
        res.is_success = false;
        return res;
    }

    bool is_ok() const { return is_success; }
    bool is_err() const { return !is_success; }

    T get_value() const { return std::get<T>(value); }
    E get_error() const { return std::get<E>(value); }
};

// Specialization for void returns (operations that just succeed or fail)
template <typename E>
class Result<void, E> {
private:
    std::variant<std::monostate, E> value;
    bool is_success;

public:
    static Result success() {
        Result res;
        res.value = std::monostate{};
        res.is_success = true;
        return res;
    }

    static Result error(E err) {
        Result res;
        res.value = std::move(err);
        res.is_success = false;
        return res;
    }

    bool is_ok() const { return is_success; }
    bool is_err() const { return !is_success; }

    E get_error() const { return std::get<E>(value); }
};

#endif // MKAPK_RESULT_HPP