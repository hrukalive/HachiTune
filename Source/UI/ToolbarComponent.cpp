#include "ToolbarComponent.h"
#include "PianoRollComponent.h"  // For EditMode enum
#include "StyledComponents.h"
#include "../Utils/Localization.h"
#include "../Utils/SvgUtils.h"
#include "BinaryData.h"

ToolbarComponent::ToolbarComponent()
{
    // Load SVG icons with white tint
    auto playIcon = SvgUtils::loadSvg(BinaryData::playline_svg, BinaryData::playline_svgSize, juce::Colours::white);
    auto pauseIcon = SvgUtils::loadSvg(BinaryData::pauseline_svg, BinaryData::pauseline_svgSize, juce::Colours::white);
    auto stopIcon = SvgUtils::loadSvg(BinaryData::stopline_svg, BinaryData::stopline_svgSize, juce::Colours::white);
    auto startIcon = SvgUtils::loadSvg(BinaryData::movestartline_svg, BinaryData::movestartline_svgSize, juce::Colours::white);
    auto endIcon = SvgUtils::loadSvg(BinaryData::moveendline_svg, BinaryData::moveendline_svgSize, juce::Colours::white);
    auto cursorIcon = SvgUtils::loadSvg(BinaryData::cursor_24_filled_svg, BinaryData::cursor_24_filled_svgSize, juce::Colours::white);
    auto pitchEditIcon = SvgUtils::loadSvg(BinaryData::pitch_edit_24_filled_svg, BinaryData::pitch_edit_24_filled_svgSize, juce::Colours::white);
    auto followIcon = SvgUtils::loadSvg(BinaryData::follow24filled_svg, BinaryData::follow24filled_svgSize, juce::Colours::white);

    playButton.setImages(playIcon.get());
    stopButton.setImages(stopIcon.get());
    goToStartButton.setImages(startIcon.get());
    goToEndButton.setImages(endIcon.get());
    selectModeButton.setImages(cursorIcon.get());
    drawModeButton.setImages(pitchEditIcon.get());
    followButton.setImages(followIcon.get());

    // Set edge indent for icon padding (makes icons smaller within button bounds)
    goToStartButton.setEdgeIndent(4);
    playButton.setEdgeIndent(6);
    stopButton.setEdgeIndent(6);
    goToEndButton.setEdgeIndent(4);
    selectModeButton.setEdgeIndent(6);
    drawModeButton.setEdgeIndent(6);
    followButton.setEdgeIndent(6);

    // Store pause icon for later use
    pauseDrawable = std::move(pauseIcon);
    playDrawable = SvgUtils::loadSvg(BinaryData::playline_svg, BinaryData::playline_svgSize, juce::Colours::white);

    // Configure buttons
    addAndMakeVisible(goToStartButton);
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(goToEndButton);
    addAndMakeVisible(selectModeButton);
    addAndMakeVisible(drawModeButton);
    addAndMakeVisible(followButton);

    // Plugin mode buttons (hidden by default)
    addChildComponent(reanalyzeButton);
    addChildComponent(araModeLabel);

    // ARA mode label style (background drawn in paint() for rounded corners)
    araModeLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    araModeLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    araModeLabel.setJustificationType(juce::Justification::centred);
    araModeLabel.setFont(juce::Font(11.0f, juce::Font::bold));

    goToStartButton.addListener(this);
    playButton.addListener(this);
    stopButton.addListener(this);
    goToEndButton.addListener(this);
    selectModeButton.addListener(this);
    drawModeButton.addListener(this);
    followButton.addListener(this);
    reanalyzeButton.addListener(this);

    // Set localized text (tooltips for icon buttons)
    selectModeButton.setTooltip(TR("toolbar.select"));
    drawModeButton.setTooltip(TR("toolbar.draw"));
    followButton.setTooltip(TR("toolbar.follow"));
    reanalyzeButton.setButtonText(TR("toolbar.reanalyze"));
    zoomLabel.setText(TR("toolbar.zoom"), juce::dontSendNotification);

    // Style reanalyze button
    reanalyzeButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF3D3D47));
    reanalyzeButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);

    // Set default active states
    selectModeButton.setActive(true);
    followButton.setActive(true);  // Follow is on by default

    // Time label with monospace font for stable width
    addAndMakeVisible(timeLabel);
    timeLabel.setText("00:00.000 / 00:00.000", juce::dontSendNotification);
    timeLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFCCCCCC));
    timeLabel.setJustificationType(juce::Justification::centred);
    timeLabel.setFont(juce::FontOptions().withName("Menlo").withHeight(14.0f).withStyle("Bold"));

    // Zoom slider
    addAndMakeVisible(zoomLabel);
    addAndMakeVisible(zoomSlider);

    zoomLabel.setColour(juce::Label::textColourId, juce::Colours::white);

    zoomSlider.setRange(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND, 1.0);
    zoomSlider.setValue(100.0);
    zoomSlider.setSkewFactorFromMidPoint(200.0);
    zoomSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    zoomSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    zoomSlider.addListener(this);

    zoomSlider.setColour(juce::Slider::backgroundColourId, juce::Colour(0xFF2D2D37));
    zoomSlider.setColour(juce::Slider::trackColourId, juce::Colour(COLOR_PRIMARY).withAlpha(0.6f));
    zoomSlider.setColour(juce::Slider::thumbColourId, juce::Colour(COLOR_PRIMARY));

    // Progress bar (hidden by default)
    addChildComponent(progressBar);
    addChildComponent(progressLabel);

    progressLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    progressLabel.setJustificationType(juce::Justification::centredLeft);
    progressBar.setColour(juce::ProgressBar::foregroundColourId, juce::Colour(COLOR_PRIMARY));
    progressBar.setColour(juce::ProgressBar::backgroundColourId, juce::Colour(0xFF2D2D37));
    
    // Status label (hidden by default)
    addChildComponent(statusLabel);
    statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setFont(juce::Font(11.0f));
}

ToolbarComponent::~ToolbarComponent() = default;

void ToolbarComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xFF1A1A24));

    // Draw rounded background for tool buttons container
    if (!toolContainerBounds.isEmpty())
    {
        g.setColour(juce::Colour(0xFF2D2D37));
        g.fillRoundedRectangle(toolContainerBounds.toFloat(), 6.0f);
    }

    // Draw rounded background for time label
    if (timeLabel.isVisible())
    {
        g.setColour(juce::Colour(0xFF2D2D37));
        g.fillRoundedRectangle(timeLabel.getBounds().toFloat(), 6.0f);
    }

    // Draw rounded background for ARA mode label
    if (pluginMode && araModeLabel.isVisible())
    {
        g.setColour(juce::Colour(COLOR_PRIMARY));
        g.fillRoundedRectangle(araModeLabel.getBounds().toFloat(), 8.0f);
    }

    // Bottom border
    g.setColour(juce::Colour(0xFF3D3D47));
    g.drawHorizontalLine(getHeight() - 1, 0, static_cast<float>(getWidth()));
}

void ToolbarComponent::resized()
{
    auto bounds = getLocalBounds().reduced(8, 4);

    // Playback controls (or plugin mode buttons)
    if (pluginMode)
    {
        araModeLabel.setBounds(bounds.removeFromLeft(90));
        bounds.removeFromLeft(8);
        reanalyzeButton.setBounds(bounds.removeFromLeft(100));
    }
    else
    {
        playButton.setBounds(bounds.removeFromLeft(28));
        bounds.removeFromLeft(4);
        stopButton.setBounds(bounds.removeFromLeft(28));
        bounds.removeFromLeft(8);
        goToStartButton.setBounds(bounds.removeFromLeft(28));
        bounds.removeFromLeft(4);
        goToEndButton.setBounds(bounds.removeFromLeft(28));
    }
    bounds.removeFromLeft(20);

    // Edit mode buttons in a container (3 buttons: select, draw, follow)
    const int toolButtonSize = 32;
    const int toolContainerPadding = 4;
    const int numToolButtons = pluginMode ? 2 : 3;  // Hide follow in plugin mode
    const int toolContainerWidth = toolButtonSize * numToolButtons + toolContainerPadding * 2;
    toolContainerBounds = bounds.removeFromLeft(toolContainerWidth).reduced(0, 2);

    auto toolArea = toolContainerBounds.reduced(toolContainerPadding, toolContainerPadding);
    selectModeButton.setBounds(toolArea.removeFromLeft(toolButtonSize));
    drawModeButton.setBounds(toolArea.removeFromLeft(toolButtonSize));
    if (!pluginMode)
        followButton.setBounds(toolArea.removeFromLeft(toolButtonSize));

    bounds.removeFromLeft(20);

    // Time display with padding for rounded background (same height as tool container)
    auto timeBounds = bounds.removeFromLeft(160).reduced(0, 2);
    timeLabel.setBounds(timeBounds);
    bounds.removeFromLeft(20);

    // Right side - zoom (slider on right, label before it)
    zoomSlider.setBounds(bounds.removeFromRight(150));
    bounds.removeFromRight(4);
    zoomLabel.setBounds(bounds.removeFromRight(50));

    // Status label (shown when not showing progress)
    if (showingStatus && !showingProgress)
    {
        statusLabel.setBounds(bounds.removeFromLeft(120));
        bounds.removeFromLeft(10);
    }
    
    // Progress bar (use the remaining middle area so it won't cover buttons)
    if (showingProgress)
    {
        auto progressArea = bounds.withWidth(std::min(300, bounds.getWidth()));

        // Vertical layout: status text above, progress bar below
        const int progressBarHeight = progressArea.getHeight() / 2;
        progressLabel.setBounds(progressArea.removeFromTop(progressArea.getHeight() - progressBarHeight));
        progressBar.setBounds(progressArea.withHeight(progressBarHeight));
    }
}

void ToolbarComponent::buttonClicked(juce::Button* button)
{
    if (button == &goToStartButton && onGoToStart)
        onGoToStart();
    else if (button == &goToEndButton && onGoToEnd)
        onGoToEnd();
    else if (button == &playButton)
    {
        if (isPlaying)
        {
            if (onPause)
                onPause();
        }
        else
        {
            if (onPlay)
                onPlay();
        }
    }
    else if (button == &stopButton && onStop)
        onStop();
    else if (button == &reanalyzeButton && onReanalyze)
        onReanalyze();
    else if (button == &selectModeButton)
    {
        setEditMode(EditMode::Select);
        if (onEditModeChanged)
            onEditModeChanged(EditMode::Select);
    }
    else if (button == &drawModeButton)
    {
        setEditMode(EditMode::Draw);
        if (onEditModeChanged)
            onEditModeChanged(EditMode::Draw);
    }
    else if (button == &followButton)
    {
        followPlayback = !followPlayback;
        followButton.setActive(followPlayback);
    }
}

void ToolbarComponent::sliderValueChanged(juce::Slider* slider)
{
    if (slider == &zoomSlider && onZoomChanged)
        onZoomChanged(static_cast<float>(slider->getValue()));
}

void ToolbarComponent::setPlaying(bool playing)
{
    isPlaying = playing;
    playButton.setImages(playing ? pauseDrawable.get() : playDrawable.get());
}

void ToolbarComponent::setCurrentTime(double time)
{
    currentTime = time;
    updateTimeDisplay();
}

void ToolbarComponent::setTotalTime(double time)
{
    totalTime = time;
    updateTimeDisplay();
}

void ToolbarComponent::setEditMode(EditMode mode)
{
    currentEditModeInt = (mode == EditMode::Draw) ? 1 : 0;
    selectModeButton.setActive(mode == EditMode::Select);
    drawModeButton.setActive(mode == EditMode::Draw);
}

void ToolbarComponent::setZoom(float pixelsPerSecond)
{
    // Update slider without triggering callback
    zoomSlider.setValue(pixelsPerSecond, juce::dontSendNotification);
}

void ToolbarComponent::showProgress(const juce::String& message)
{
    showingProgress = true;
    progressLabel.setText(message, juce::dontSendNotification);
    progressLabel.setVisible(true);
    progressBar.setVisible(true);
    progressValue = -1.0;  // Indeterminate
    resized();
    repaint();
}

void ToolbarComponent::hideProgress()
{
    showingProgress = false;
    progressLabel.setVisible(false);
    progressBar.setVisible(false);
    resized();
    repaint();
}

void ToolbarComponent::setProgress(float progress)
{
    if (progress < 0)
        progressValue = -1.0;  // Indeterminate
    else
        progressValue = static_cast<double>(juce::jlimit(0.0f, 1.0f, progress));
}

void ToolbarComponent::setStatusMessage(const juce::String& message)
{
    if (message.isEmpty())
    {
        showingStatus = false;
        statusLabel.setVisible(false);
    }
    else
    {
        showingStatus = true;
        statusLabel.setText(message, juce::dontSendNotification);
        statusLabel.setVisible(true);
    }
    resized();
    repaint();
}

void ToolbarComponent::updateTimeDisplay()
{
    timeLabel.setText(formatTime(currentTime) + " / " + formatTime(totalTime),
                      juce::dontSendNotification);
}

juce::String ToolbarComponent::formatTime(double seconds)
{
    int mins = static_cast<int>(seconds) / 60;
    int secs = static_cast<int>(seconds) % 60;
    int ms = static_cast<int>((seconds - std::floor(seconds)) * 1000);

    return juce::String::formatted("%02d:%02d.%03d", mins, secs, ms);
}

void ToolbarComponent::mouseDown(const juce::MouseEvent& e)
{
#if JUCE_MAC
    if (auto* window = getTopLevelComponent())
        dragger.startDraggingComponent(window, e.getEventRelativeTo(window));
#else
    juce::ignoreUnused(e);
#endif
}

void ToolbarComponent::mouseDrag(const juce::MouseEvent& e)
{
#if JUCE_MAC
    if (auto* window = getTopLevelComponent())
        dragger.dragComponent(window, e.getEventRelativeTo(window), nullptr);
#else
    juce::ignoreUnused(e);
#endif
}

void ToolbarComponent::mouseDoubleClick(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
}

void ToolbarComponent::setPluginMode(bool isPlugin)
{
    pluginMode = isPlugin;

    goToStartButton.setVisible(!isPlugin);
    playButton.setVisible(!isPlugin);
    stopButton.setVisible(!isPlugin);
    goToEndButton.setVisible(!isPlugin);
    reanalyzeButton.setVisible(isPlugin);
    araModeLabel.setVisible(isPlugin);

    // In plugin mode, hide follow button (host controls playback)
    followButton.setVisible(!isPlugin);

    resized();
}

void ToolbarComponent::setARAMode(bool isARA)
{
    araMode = isARA;
    araModeLabel.setText(isARA ? "ARA Mode" : "Non-ARA", juce::dontSendNotification);
}
