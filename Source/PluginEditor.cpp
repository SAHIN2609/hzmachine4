#include "PluginEditor.h"
#include <UIData.h>
#include <cstring>
#include <limits>

static std::vector<std::byte> toBytes (const char* data, size_t size)
{
    std::vector<std::byte> v (size);
    if (size > 0)
        std::memcpy (v.data(), data, size);
    return v;
}

static juce::WebBrowserComponent::Resource jsonResource (const juce::var& v)
{
    const auto text = juce::JSON::toString (v, true);
    const auto utf8 = text.toUTF8();
    return { toBytes (utf8.getAddress(), utf8.sizeInBytes() - 1), "application/json" };
}

void SHZEditor::setParamValue (const juce::String& id, float v)
{
    for (int i = 0; i < Id::Count; ++i)
    {
        if (id == kParams[i].id)
        {
            if (auto* p = proc.apvts.getParameter (id))
            {
                p->setValueNotifyingHost (p->convertTo0to1 (v));
                lastSent[(size_t) i] = rawPtrs[(size_t) i]->load();   // the UI already has this value
            }
            return;
        }
    }
}

SHZEditor::SHZEditor (SHZProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    for (int i = 0; i < Id::Count; ++i)
    {
        rawPtrs[(size_t) i] = proc.apvts.getRawParameterValue (kParams[i].id);
        lastSent[(size_t) i] = rawPtrs[(size_t) i]->load();
    }

    using Opt = juce::WebBrowserComponent::Options;
    Opt opts = Opt{}
       #if JUCE_WINDOWS
        .withBackend (Opt::Backend::webview2)   // default on Windows is Internet Explorer -> blank page
       #endif
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withWinWebView2Options (Opt::WinWebView2{}
            .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                     .getChildFile ("SISHHIN_HZ_MACHINE_WebView2")))
        .withResourceProvider ([this] (const juce::String& url) { return getResource (url); });

    web = std::make_unique<juce::WebBrowserComponent> (opts);
    addAndMakeVisible (*web);
    web->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    setResizable (true, true);
    setResizeLimits (720, 520, 1700, 1200);
    setSize (1080, 760);
}

SHZEditor::~SHZEditor() {}

void SHZEditor::resized()
{
    if (web != nullptr)
        web->setBounds (getLocalBounds());
}

// UI <-> plugin protocol: the page requests /shz/... and the plugin answers with JSON.
std::optional<juce::WebBrowserComponent::Resource> SHZEditor::handleApi (const juce::String& path)
{
    const auto parts = juce::StringArray::fromTokens (path.fromFirstOccurrenceOf ("/shz/", false, false), "/", "");
    const auto cmd = parts[0];

    if (cmd == "set" && parts.size() >= 3)
    {
        setParamValue (parts[1], parts[2].getFloatValue());
    }
    else if (cmd == "sets" && parts.size() >= 2)
    {
        for (const auto& pair : juce::StringArray::fromTokens (parts[1], ",", ""))
            if (pair.contains (":"))
                setParamValue (pair.upToFirstOccurrenceOf (":", false, false),
                               pair.fromFirstOccurrenceOf (":", false, false).getFloatValue());
    }
    else if (cmd == "g" && parts.size() >= 3)
    {
        if (auto* prm = proc.apvts.getParameter (parts[1]))
        {
            if (parts[2].getIntValue() != 0) prm->beginChangeGesture();
            else                             prm->endChangeGesture();
        }
    }
    else if (cmd == "all")
    {
        auto* o = new juce::DynamicObject();
        for (int i = 0; i < Id::Count; ++i)
        {
            o->setProperty (juce::Identifier (kParams[i].id), (double) rawPtrs[(size_t) i]->load());
            lastSent[(size_t) i] = rawPtrs[(size_t) i]->load();
        }
        return jsonResource (juce::var (o));
    }
    else if (cmd == "poll")
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("in",  (double) proc.engine.inPeak.load());
        o->setProperty ("out", (double) proc.engine.outPeak.load());
        o->setProperty ("ir",  proc.getIRName());

        auto* changed = new juce::DynamicObject();
        for (int i = 0; i < Id::Count; ++i)
        {
            const float v = rawPtrs[(size_t) i]->load();
            if (v != lastSent[(size_t) i])
            {
                lastSent[(size_t) i] = v;
                changed->setProperty (juce::Identifier (kParams[i].id), (double) v);
            }
        }
        o->setProperty ("p", juce::var (changed));
        return jsonResource (juce::var (o));
    }
    else if (cmd == "loadIR")
    {
        juce::Component::SafePointer<SHZEditor> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe == nullptr) return;
            safe->chooser = std::make_unique<juce::FileChooser> ("Load impulse response", juce::File(),
                                                                 "*.wav;*.aif;*.aiff;*.flac");
            safe->chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [safe] (const juce::FileChooser& fc)
                {
                    if (safe == nullptr) return;
                    const auto f = fc.getResult();
                    if (f.existsAsFile())
                        safe->proc.loadIRFile (f);
                });
        });
    }
    else if (cmd == "clearIR")
    {
        proc.clearIR();
    }

    auto* ok = new juce::DynamicObject();
    ok->setProperty ("ok", true);
    return jsonResource (juce::var (ok));
}

std::optional<juce::WebBrowserComponent::Resource> SHZEditor::getResource (const juce::String& url)
{
    const auto path = url.upToFirstOccurrenceOf ("?", false, false);

    if (path.startsWith ("/shz/"))
        return handleApi (path);

    if (path == "/" || path.isEmpty() || path == "/index.html")
        return juce::WebBrowserComponent::Resource { toBytes (UIData::index_html, (size_t) UIData::index_htmlSize), "text/html" };

    return std::nullopt;
}
