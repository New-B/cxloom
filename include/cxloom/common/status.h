#pragma once

#include <string>
#include <utility>

namespace cxloom {

enum class StatusCode {
    kOk = 0,
    kInvalidArgument,
    kNotFound,
    kAlreadyExists,
    kUnavailable,
    kFailedPrecondition,
    kUnimplemented,
    kInternal,
};

class Status {
public:
    Status() = default;
    Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

    static Status Ok() { return Status(); }
    static Status InvalidArgument(std::string message) { return {StatusCode::kInvalidArgument, std::move(message)}; }
    static Status NotFound(std::string message) { return {StatusCode::kNotFound, std::move(message)}; }
    static Status AlreadyExists(std::string message) { return {StatusCode::kAlreadyExists, std::move(message)}; }
    static Status Unavailable(std::string message) { return {StatusCode::kUnavailable, std::move(message)}; }
    static Status FailedPrecondition(std::string message) { return {StatusCode::kFailedPrecondition, std::move(message)}; }
    static Status Unimplemented(std::string message) { return {StatusCode::kUnimplemented, std::move(message)}; }
    static Status Internal(std::string message) { return {StatusCode::kInternal, std::move(message)}; }

    bool ok() const { return code_ == StatusCode::kOk; }
    StatusCode code() const { return code_; }
    const std::string& message() const { return message_; }

private:
    StatusCode code_ {StatusCode::kOk};
    std::string message_;
};

template <typename T>
class Result {
public:
    Result(const T& value) : value_(value), status_(Status::Ok()), has_value_(true) {}
    Result(T&& value) : value_(std::move(value)), status_(Status::Ok()), has_value_(true) {}
    Result(Status status) : status_(std::move(status)), has_value_(false) {}

    bool ok() const { return has_value_ && status_.ok(); }
    const Status& status() const { return status_; }
    const T& value() const { return value_; }
    T& value() { return value_; }

private:
    T value_ {};
    Status status_ {};
    bool has_value_ {false};
};

}  // namespace cxloom

