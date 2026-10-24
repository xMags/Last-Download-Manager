#include "core/FailureText.h"

namespace ldm::core {

std::string_view DescribeFailureReason(FailureReason reason) noexcept
{
    switch (reason) {
    case FailureReason::Dns:
        return "Server name could not be found";
    case FailureReason::ConnectionRefused:
        return "Connection refused";
    case FailureReason::ConnectionReset:
        return "Connection lost";
    case FailureReason::Timeout:
        return "Connection timed out";
    case FailureReason::Proxy:
        return "Proxy server failed";
    case FailureReason::Tls:
        return "Secure connection failed";
    case FailureReason::AuthRequired:
        return "Login required";
    case FailureReason::ProxyAuthRequired:
        return "Proxy login required";
    case FailureReason::NotFound:
        return "File not found on the server";
    case FailureReason::HttpError:
        return "Server error";
    case FailureReason::HtmlInsteadOfFile:
        return "Server sent a web page instead of the file";
    case FailureReason::ResourceChanged:
        return "File changed on the server";
    case FailureReason::InconsistentContent:
        return "Server sent inconsistent data";
    case FailureReason::ResumeUnsupported:
        return "Server cannot resume this download";
    case FailureReason::PartialFileMissing:
        return "Downloaded part is missing";
    case FailureReason::DiskFull:
        return "Disk is full";
    case FailureReason::FileTooLarge:
        return "File is too large for the target drive";
    case FailureReason::DiskError:
        return "Disk error";
    case FailureReason::PermissionDenied:
        return "Access denied";
    case FailureReason::Protocol:
        return "Invalid server response";
    case FailureReason::PrivateAddress:
        return "The stream points into your own network, which LDM refuses for streams from the internet";
    case FailureReason::UnsupportedStream:
        return "This stream cannot be downloaded";
    case FailureReason::Internal:
        return "Internal error";
    }
    return "Error";
}

std::string DescribeFailure(const TransferFailure& failure)
{
    std::string text(DescribeFailureReason(failure.reason));
    if (!failure.message.empty()) {
        text += " (" + failure.message + ")";
    }
    return text;
}

}  // namespace ldm::core
