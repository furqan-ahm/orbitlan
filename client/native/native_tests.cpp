#include "common.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

}  // namespace

int main() {
    using namespace orbitlan;

    const std::string utf8 = "OrbitLan: Furqan / \xE5\x9C\xB0\xE7\x90\x83";
    Check(WideToUtf8(Utf8ToWide(utf8)) == utf8, "UTF-8 conversion round trip");

    const std::string encoded = Base64Encode("room\twith delimiters\nand lines");
    std::string decoded;
    Check(!encoded.empty() && Base64Decode(encoded, decoded), "base64 decode succeeds");
    Check(decoded == "room\twith delimiters\nand lines", "base64 round trip");
    Check(!Base64Decode("not valid base64%%%", decoded), "invalid base64 is rejected");

    const auto fields = Split("CONNECT\ta\tb\t", '\t');
    Check(fields.size() == 4 && fields[3].empty(), "protocol split preserves empty final field");

    Check(QuoteArgument(L"plain") == L"plain", "plain argument remains plain");
    Check(QuoteArgument(L"two words") == L"\"two words\"", "spaces are quoted");
    Check(QuoteArgument(L"quoted\"value") == L"\"quoted\\\"value\"", "quotes are escaped");

    if (failures == 0) std::cout << "OrbitLan native tests passed\n";
    return failures == 0 ? 0 : 1;
}
