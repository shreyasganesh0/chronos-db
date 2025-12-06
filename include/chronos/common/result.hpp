#pragma once

#include <variant>
#include <iostream>

namespace chronos {

enum class Error : int {

	Ok = 0,
	GenericError = 1,
	IOError = 2,
};

template <typename T>
class [[nodiscard]] Result {
public:

	Result(T value) : content_(std::move(value)) {}

	Result(Error error) : content_(error) {}

	bool is_ok() const { return std::holds_alternative<T>(content_);}
	bool is_err() const { return std::holds_alternative<Error>(content_);}
	
	T* operator->() {

		return std::get_if<T>(&content_);
	}

	const T* operator->() const {

		return std::get_if<T>(&content_);
	}

	T& value() {
		if (!is_ok()) {
            std::cerr << "CRITICAL: Attempted to unwrap Error Result!" << std::endl;
            std::abort(); 
        }
		return *std::get_if<T>(&content_);
	}

	Error error() const {

		return *std::get_if<Error>(&content_);
	}
private:

	std::variant<Error, T> content_;
};

template <>
class [[nodiscard]]Result<void> {

public:

	Result() : error_(Error::Ok) {}
	Result(Error error) : error_(error) {}

	static Result success() {return Result();}

	bool is_ok() const {return error_ == Error::Ok;}
	bool is_err() const {return error_ != Error::Ok;}

	Error error() const {return error_;}

private:
	Error error_;
};
}
