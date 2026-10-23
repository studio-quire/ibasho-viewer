/**
 * @file   ibasholimits.cpp
 * @brief  Hard limits that hold however the viewer is driven (LEAP APIs, or UI clicks and keys sent
 *         over LEAP): a daily cap on outgoing chat lines and IMs, the spending caps on the UI's own
 *         payment and purchase paths, no land or L$ purchases unless the person allows them, and a
 *         built-in browser that doesn't open files or pages on this machine or its network.
 *         Written by Claude Opus 5.5, 2026-10-22 and 2026-10-23.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Ibasho viewer source code: an addition to the Second Life Viewer Source Code
 * (Linden Research, Inc.), written for Ibasho by Claude (Anthropic) models.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "ibashocommon.h"
#include "ibashohooks.h"

#include "lldir.h"
#include "llnotificationsutil.h"
#include "llsdutil.h"
#include "llstring.h"
#include "lltimer.h"
#include "lluri.h"
#include "llviewercontrol.h"
#include "llviewernetwork.h"

#include <cctype>
#include <fstream>

namespace
{

// Today's count of outgoing chat lines and IMs, by Second Life time (Pacific) date, kept in a small file
// in the settings folder so a restart (or a crash) doesn't reset it.
struct MessageCount
{
    std::string date;
    S32 count = 0;
    bool loaded = false;
};
MessageCount sMessages;

std::string countFile()
{
    return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "ibasho_messages.txt");
}

std::string sltDate()
{
    struct tm* t = utc_to_pacific_time(time(nullptr), LLStringOps::getPacificDaylightTime());
    return llformat("%04d-%02d-%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
}

void rollDate()
{
    if (!sMessages.loaded)
    {
        sMessages.loaded = true;
        std::ifstream in(countFile());
        in >> sMessages.date >> sMessages.count;
    }
    std::string today = sltDate();
    if (today != sMessages.date)
    {
        sMessages.date = today;
        sMessages.count = 0;
    }
}

void saveCount()
{
    std::ofstream out(countFile(), std::ios::trunc);
    out << sMessages.date << ' ' << sMessages.count << '\n';
}

// The built-in browser's URL check reads a URL as a browser would: tabs and newlines anywhere are
// ignored, and so are spaces and control characters at either end; case doesn't matter.
std::string browserForm(const std::string& url)
{
    std::string s;
    for (char c : url)
    {
        if (c != '\t' && c != '\n' && c != '\r') s += c;
    }
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') ++b;
    while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
    s = s.substr(b, e - b);
    LLStringUtil::toLower(s);
    while (s.compare(0, 12, "view-source:") == 0) s.erase(0, 12);
    return s;
}

// The scheme, or "" if there's none ("127.0.0.1:8080/" has none; "localhost:8080" looks like one).
std::string schemeOf(const std::string& s)
{
    size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0 || !std::isalpha((unsigned char)s[0])) return "";
    for (size_t i = 0; i < colon; ++i)
    {
        if (!std::isalnum((unsigned char)s[i]) && s[i] != '+' && s[i] != '-' && s[i] != '.') return "";
    }
    return s.substr(0, colon);
}

// host[:port] after the slashes (either kind, or none) that follow the scheme, without user info,
// percent-decoded.
std::string authorityOf(const std::string& rest)
{
    size_t b = rest.find_first_not_of("/\\");
    if (b == std::string::npos) return "";
    std::string a = rest.substr(b, rest.find_first_of("/\\?#", b) - b);
    size_t at = a.rfind('@');
    if (at != std::string::npos) a.erase(0, at + 1);
    a = LLURI::unescape(a);
    LLStringUtil::toLower(a);
    return a;
}

std::string hostOf(const std::string& authority)
{
    std::string h;
    size_t close = authority.find(']');
    if (!authority.empty() && authority[0] == '[')
        h = close == std::string::npos ? authority : authority.substr(0, close + 1);
    else
        h = authority.substr(0, authority.find(':'));
    while (!h.empty() && h.back() == '.') h.pop_back();
    return h;
}

// An IPv4 host as browsers read one: one to four parts, each decimal, octal (leading 0) or hex (0x), the
// last filling the remaining bytes, so 127.1, 0x7f.0.0.1 and 2130706433 are all 127.0.0.1.
bool ipv4Of(const std::string& host, U32& addr)
{
    std::vector<U64> parts;
    size_t start = 0;
    while (true)
    {
        size_t dot = host.find('.', start);
        std::string part = host.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (part.empty() || parts.size() == 4) return false;
        U64 base = 10, v = 0;
        size_t i = 0;
        if (part.size() > 1 && part[0] == '0' && part[1] == 'x') { base = 16; i = 2; }
        else if (part.size() > 1 && part[0] == '0') { base = 8; i = 1; }
        for (; i < part.size(); ++i)
        {
            char c = part[i];
            U64 d = (c >= '0' && c <= '9') ? U64(c - '0') : (c >= 'a' && c <= 'f') ? U64(c - 'a' + 10) : 99;
            if (d >= base) return false;
            v = v * base + d;
            if (v > 0xffffffffULL) return false;
        }
        parts.push_back(v);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    U64 a = 0;
    for (size_t k = 0; k + 1 < parts.size(); ++k)
    {
        if (parts[k] > 255) return false;
        a = (a << 8) | parts[k];
    }
    size_t rest = 5 - parts.size();  // bytes left for the last part
    if (parts.back() >> (8 * rest)) return false;
    addr = (U32)((a << (8 * rest)) | parts.back());
    return true;
}

bool isLocalHost(const std::string& host)
{
    if (host.empty()) return false;
    if (host[0] == '[') return true;  // any IPv6 literal (::1, fc00::/7, fe80::/10, mapped IPv4...)
    if (host == "localhost" || (host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0))
        return true;
    U32 a;
    if (!ipv4Of(host, a)) return false;
    U32 a0 = a >> 24, a1 = (a >> 16) & 0xff;
    return a0 == 0 || a0 == 10 || a0 == 127 || a0 >= 224                // this network, private, loopback, multicast
        || (a0 == 100 && (a1 & 0xc0) == 64)                             // shared (carrier NAT, VPN meshes)
        || (a0 == 169 && a1 == 254) || (a0 == 172 && (a1 & 0xf0) == 16) || (a0 == 192 && a1 == 168);
}

// scheme://host[:port], or "" without a scheme.
std::string originOf(const std::string& url)
{
    std::string s = browserForm(url), scheme = schemeOf(s);
    return scheme.empty() ? "" : scheme + "://" + authorityOf(s.substr(scheme.size() + 1));
}

// "file" for a file, the host for a page on this machine or its network, else "".
std::string localTarget(const std::string& url)
{
    std::string s = browserForm(url), scheme = schemeOf(s);
    if (scheme == "file" || scheme == "filesystem") return "file";
    bool web = scheme == "http" || scheme == "https" || scheme == "ws" || scheme == "wss" || scheme == "ftp";
    // other schemes are also read as a bare host, as a browser may ("localhost:8080")
    std::string host = hostOf(authorityOf(web ? s.substr(scheme.size() + 1) : s));
    return isLocalHost(host) ? host : "";
}

// The grid's own login page and login service: the person chose the grid, wherever it runs.
bool isGridPage(const std::string& url)
{
    std::string origin = originOf(url);
    std::vector<std::string> pages;
    LLGridManager::getInstance()->getLoginURIs(pages);
    pages.push_back(LLGridManager::getInstance()->getLoginPage());
    for (const std::string& page : pages)
    {
        if (!origin.empty() && origin == originOf(page)) return true;
    }
    return false;
}

} // namespace

namespace ibasho
{

void refuse(const std::string& what, const std::string& reason)
{
    LL_WARNS("Ibasho") << reason << LL_ENDL;
    LLNotificationsUtil::add("SystemMessageTip", llsd::map("MESSAGE", reason));
    postEvent("refused", llsd::map("what", what, "reason", reason));
}

bool allowOutgoingMessage(const std::string& kind)
{
    rollDate();
    S32 max = gSavedSettings.getS32("IbashoMaxMessagesPerDay");
    if (max > 0 && sMessages.count >= max)
    {
        refuse(kind, llformat("Ibasho: this %s wasn't sent. Today's limit of %d chat lines and IMs "
                              "(IbashoMaxMessagesPerDay, counted by Second Life time) is reached.",
                              kind.c_str(), max));
        return false;
    }
    ++sMessages.count;
    saveCount();
    return true;
}

LLSD messageCount()
{
    rollDate();
    S32 max = gSavedSettings.getS32("IbashoMaxMessagesPerDay");
    return llsd::map("date", sMessages.date, "count", sMessages.count, "max", max,
                     "left", max > 0 ? LLSD(llmax(0, max - sMessages.count)) : LLSD());
}

bool allowSpend(S32 amount, const std::string& what)
{
    std::string refusal = spendRefusal(amount);
    if (!refusal.empty())
    {
        refuse(what, "Ibasho: " + what + " refused: " + refusal + ".");
        return false;
    }
    recordSpend(amount);
    return true;
}

bool allowLandBuy()
{
    if (gSavedSettings.getBOOL("IbashoAllowLandBuy"))
        return true;
    refuse("land", "Ibasho: buying land is off in this viewer (setting IbashoAllowLandBuy).");
    return false;
}

bool allowCurrencyBuy()
{
    if (gSavedSettings.getBOOL("IbashoAllowCurrencyBuy"))
        return true;
    refuse("currency", "Ibasho: buying L$ with real money is off in this viewer (setting IbashoAllowCurrencyBuy).");
    return false;
}

bool allowBrowserURL(const std::string& url)
{
    std::string target = localTarget(url);
    if (target.empty() || (target != "file" && isGridPage(url)))
        return true;
    std::string reason = target == "file"
        ? "Ibasho: the built-in browser doesn't open files on this computer."
        : "Ibasho: the built-in browser doesn't open pages on this computer or its network (" + target + ").";
    // Media retries a navigation now and then, and a page can redirect in a loop: once a minute is enough.
    static std::string sLast;
    static F64 sLastAt = 0.0;
    F64 now = LLTimer::getTotalSeconds().value();
    if (reason != sLast || now - sLastAt > 60.0)
    {
        sLast = reason;
        sLastAt = now;
        refuse("browser", reason);
    }
    return false;
}

} // namespace ibasho

// LLPluginClassMedia (llplugin) asks this before any media plugin loads a URL, and when a page arrives at one.
extern bool (*gIbashoAllowURL)(const std::string& url); // llpluginclassmedia.cpp
static const bool sIbashoURLHook = (gIbashoAllowURL = ibasho::allowBrowserURL, true);
