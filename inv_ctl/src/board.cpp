#include "board.hpp"

#include <cstdio>
#include <string_view>
#include <utility>

using namespace std;

namespace {

/* DT string properties are exactly one NUL-terminated string and nothing
 * else (bench od -c capture, Zero W): stop at the NUL, but also tolerate a
 * trailing newline so echo-created or captured fixtures replay identically
 * in the unit tests. */
string parse_dt_string(FILE * f)
{
    string s;
    for (int c = fgetc(f); c != EOF && c != '\0' && c != '\n'; c = fgetc(f))
        s.push_back(static_cast<char>(c));
    return s;
}

string read_dt(const char * path)
{
    FILE * f = fopen(path, "re");
    if (!f)
        return {};
    string s = parse_dt_string(f);
    fclose(f);
    return s;
}

/* The model property is the authority: with no DT at all (laptop) or a
 * foreign board we return empty BoardInfo and the panel shows nothing. */
bool looks_like_raspberry_pi(const string_view model)
{
    return model.rfind("Raspberry Pi", 0) == 0;
}

} // namespace

BoardInfo board_query()
{
    string model = read_dt("/sys/firmware/devicetree/base/model");
    if (!looks_like_raspberry_pi(model))
        return {};
    return BoardInfo{
        .model = move(model),
        .serial = read_dt("/sys/firmware/devicetree/base/serial-number"),
    };
}
