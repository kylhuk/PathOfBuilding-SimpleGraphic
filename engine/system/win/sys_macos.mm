#include <string_view>
#include <CoreFoundation/CFBundle.h>
#include <ApplicationServices/ApplicationServices.h>

#include "common.h"

const char* PlatformOpenURL(const char* textUrl)
{
    if (!textUrl || !*textUrl) {
        return AllocString("Did not open URL: the URL is empty.");
    }
    std::string_view urlView = textUrl;
    CFURLRef url = CFURLCreateWithBytes(nullptr, (const UInt8*)urlView.data(), urlView.size(), kCFStringEncodingUTF8, nullptr);
    if (!url) {
        return AllocString("Did not open URL: the URL could not be parsed.");
    }
    OSStatus result = LSOpenCFURLRef(url, nullptr);
    CFRelease(url);
    if (result != noErr) {
        return AllocString("Did not open URL: Launch Services returned an error.");
    }
    return nullptr;
}
