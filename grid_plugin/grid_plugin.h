/*
==============================================================================

BEGIN_JUCE_MODULE_DECLARATION

   ID:            grid_plugin
   vendor:        WolfSound
   version:       1.1.0
   name:          Grid Buttons Plugin
   description:   Core of the grid buttons plugin (UI-only skeleton)
   dependencies:  juce_audio_processors, juce_gui_basics, juce_graphics

   website:       https://thewolfsound.com
   license:       MIT

END_JUCE_MODULE_DECLARATION

==============================================================================
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_core/juce_core.h>

#include "include/Grid/PluginProcessor.h"
#include "include/Grid/PluginEditor.h"