#include <fpp-pch.h>

#include <string>
#include <vector>

#include <unistd.h>
#include <termios.h>

#include "mediadetails.h"
#include "common.h"
#include "settings.h"
#include "Plugin.h"
#include "log.h"

class FPPPixelRadioPlugin : public FPPPlugin {
public:
    std::string baseURL;

    FPPPixelRadioPlugin() : FPPPlugin("fpp-PixelRadio") {
        setDefaultSettings();
        baseURL = "http://" + settings["IPAddress"] + ":" + settings["Port"] + "/cmd?";

        std::string freq = settings["Frequency"];
        replaceAll(freq, ".", "");

        std::string resp;
        urlGet(baseURL + "freq=" + freq, resp);
        urlGet(baseURL + "pic=" + settings["ProgramType"], resp);

        formatAndSendText(settings["StationID"], "", "", "", 0, true);
        formatAndSendText(settings["RDS"], "", "", "", 0, false);
    }
    virtual ~FPPPixelRadioPlugin() {
    }
    
    void startAction() {
        std::string resp;
        if (settings["IdleAction"] == "1") {
            urlGet(baseURL + "mute=off", resp);
        } else if (settings["IdleAction"] == "2") {
            urlGet(baseURL + "start=1", resp);
        }
    }
   
    void stopAction() {
        std::string resp;
        if (settings["IdleAction"] == "1") {
            urlGet(baseURL + "mute=on", resp);
        } else if (settings["IdleAction"] == "2") {
            urlGet(baseURL + "stop=1", resp);
        }
    }
    
    static void padTo(std::string &s, int l) {
        size_t n = l - s.size();
        if (n) {
            s.append(n, ' ');
        }
    }

    void formatAndSendText(const std::string &text, const std::string &artist, const std::string &title, const std::string &album, int length, bool station) {
        std::string output;
        
        int artistIdx = -1;
        int titleIdx = -1;
        int albumIdx = -1;

        for (int x = 0; x < text.length(); x++) {
            if (text[x] == '[') {
                if (artist == "" && title == "") {
                    while (text[x] != ']' && x < text.length()) {
                        x++;
                    }
                }
            } else if (text[x] == ']') {
                //nothing
            } else if (text[x] == '{') {
                const static std::string ARTIST = "{Artist}";
                const static std::string TITLE = "{Title}";
                const static std::string ALBUM = "{Album}";
                std::string subs = text.substr(x);
                if (subs.rfind(ARTIST) == 0) {
                    artistIdx = output.length();
                    x += ARTIST.length() - 1;
                    output += artist;
                } else if (subs.rfind(TITLE) == 0) {
                    titleIdx = output.length();
                    x += TITLE.length() - 1;
                    output += title;
                } else if (subs.rfind(ALBUM) == 0) {
                    titleIdx = output.length();
                    x += ALBUM.length() - 1;
                    output += album;
                } else {
                    output += text[x];
                }
            } else {
                output += text[x];
            }
        }
        if (station) {
            LogDebug(VB_PLUGIN, "Setting RDS Station text to \"%s\"\n", output.c_str());
            std::vector<std::string> fragments;
            while (output.size()) {
                if (output.size() <= 8) {
                    padTo(output, 8);
                    fragments.push_back(output);
                    output.clear();
                } else {
                    std::string lft = output.substr(0, 8);
                    padTo(lft, 8);
                    output = output.substr(8);
                    fragments.push_back(lft);
                }
            }
            if (fragments.empty()) {
                std::string m = "        ";
                fragments.push_back(m);
            }
            //FIXME - PixelRadio only supports a single fragment?
            std::string resp;
            urlGet(baseURL + "psn=" + fragments[0], resp);
        } else {
            LogDebug(VB_PLUGIN, "Setting RDS text to \"%s\"\n", output.c_str());
            std::string resp;
            urlGet(baseURL + "rtm=" + output, resp);
        }
    }
    

    virtual void playlistCallback(const Json::Value &playlist, const std::string &action, const std::string &section, int item) {
        if (action == "stop") {
            formatAndSendText(settings["StationID"], "", "", "", 0, true);
            formatAndSendText(settings["RDS"], "", "", "", 0, false);
        }
        if (action == "start") {
            startAction();
        } else if (action == "stop") {
            stopAction();
        }
        
    }
    virtual void mediaCallback(const Json::Value &playlist, const MediaDetails &mediaDetails) {
        std::string title = mediaDetails.title;
        std::string artist = mediaDetails.artist;
        std::string album = mediaDetails.album;
        int track = mediaDetails.track;
        int length = mediaDetails.length;
        
        std::string type = playlist["currentEntry"]["type"].asString();
        if (type != "both" && type != "media") {
            title = "";
            artist = "";
            album = "";
        }
        
        formatAndSendText(settings["StationID"], artist, title, album, length, true);
        formatAndSendText(settings["RDS"], artist, title, album, length, false);
    }
    
    
    void setDefaultSettings() {
        setIfNotFound("Frequency", "87.9");
        setIfNotFound("IdleAction", "0");
        setIfNotFound("IPAddress", "");
        setIfNotFound("Port", "80");

        setIfNotFound("StationID", "Merry   Christ- mas", true);
        setIfNotFound("RDS", "[{Artist} - {Title}]", true);
        setIfNotFound("ProgramType", "0");

    }
    void setIfNotFound(const std::string &s, const std::string &v, bool emptyAllowed = false) {
        if (settings.find(s) == settings.end()) {
            settings[s] = v;
        } else if (!emptyAllowed && settings[s] == "") {
            settings[s] = v;
        }
        LogDebug(VB_PLUGIN, "Setting \"%s\": \"%s\"\n", s.c_str(), settings[s].c_str());
    }
};


extern "C" {
    FPPPlugin *createPlugin() {
        return new FPPPixelRadioPlugin();
    }
}
