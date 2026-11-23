/*
 *  CreatureSpeech.h
 *  openc2e
 *
 *  C1/C2 creatures speaking under a mind: what a creature of a given age says
 *  for a meaning the mind (or SAYN) chooses, in the words it has learned, in a
 *  speech bubble and its voice; and how creatures learn words.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 */

#pragma once

#include "AgentRef.h"

#include <deque>
#include <string>
#include <vector>

class Agent;
class oldCreature;

// Words. Creatures are born knowing none. The words are the game's own word
// list (engine.wordlist, from the player's copy, in their language); each
// creature keeps how familiar each word is (oldCreature::vocab, 0-255) and
// knows it from WORD_KNOWN. Hearing a word makes it more familiar, by an
// amount for the source, scaled by life stage (children learn fastest).
// A word not known is shown in the creature's own tongue (Nornish, or
// grendel and ettin dialects): the syllables its voice speaks for the word,
// as the game's voice files name them (the voice turns each three-letter
// group of the word into a recorded syllable, so a word always sounds the
// same). The voice speaks every word that way, learned or not; learning
// changes what the bubble shows.
const int WORD_KNOWN = 128;
const int HEAR_MACHINE = 64; // a learning machine's word, looked at or close by (one press, two hearings, teaches it)
const int HEAR_MACHINE_FAR = 32; // a learning machine's word, heard from further off
const int HEAR_HAND = 48; // a word the hand typed
const int HEAR_CREATURE = 24; // a word another creature said

bool knowsWord(oldCreature* c, int word);
void hearWord(oldCreature* c, int word, int amount);
void learnAllWords(oldCreature* c); // Stone of Knowledge (SYS: CMND 32777), VOCB
int wordsKnown(oldCreature* c);

// Names of a creature's own (oldCreature::ownNames): the category a word
// names, or -1; its name for a category, or "".
int ownNameMeans(oldCreature* c, const std::string& word);
std::string ownNameFor(oldCreature* c, int category);
int ownNamesKnown(oldCreature* c);
// it hears `word` as a name for things of that category
void hearOwnName(oldCreature* c, const std::string& word, int category, int amount);

// Creatures within earshot of the speaker (awake, alive, not the speaker)
// hear each word of the game's word list in the text (the hand's typing),
// or the given words (another creature's speech).
void hearSpeech(Agent* speaker, const std::string& text, int amount);
void hearWords(Agent* speaker, const std::vector<int>& words, int amount);

// The syllables a creature's voice speaks for a word, as shown in a bubble
// ("doh-rah-doh"); empty if it has no voice.
std::string nativeWord(Agent* creature, const std::string& word);

// A learning machine (blackboard) says word index `word`: creatures looking
// at it or right by it learn it at HEAR_MACHINE; when it speaks aloud, the
// rest in earshot at HEAR_MACHINE_FAR.
void hearMachineWord(Agent* machine, int word, bool aloud);

// Meanings (our own small set of intents):
//   "need:D:L"   wants drive D relieved (C2 drive number), at level L 0-255
//   "ow"         hurt just now
//   "yum"        a good meal
//   "grr"        angry
//   "love"       a kiss that pleased
//   "greet:G"    a creature of genus G (1 norn, 2 grendel, 3 ettin) came into view
//   "babble"     nothing in particular
//   "words:A:B"  these word-list words, in order (the llm mind; up to four,
//                a child one)
// Babies only babble and cry; children say one word; the grown say short
// phrases; the senile trail off. Words the creature has not learned are
// shown in its own tongue.
struct Utterance {
	std::string shown; // in the bubble
	std::string spoken; // what the voice speaks (the words themselves)
	std::vector<int> learned; // the learned words in it, for listeners
	std::vector<std::pair<std::string, int>> own; // and the creature's own names for things (ownNames)
};
Utterance phraseFor(const std::string& meaning, Agent* creature, oldCreature* c);
// One word of the game's word list (C2's SAY#): shown as the creature knows
// it (or in its own tongue), and heard by those in earshot.
Utterance wordUtterance(Agent* creature, oldCreature* c, int word);

// View > Speech Bubbles: whether creatures' speech shows in bubbles (the
// voice and learning go on either way). Remembered in the user data
// folder's settings.txt ("speech-bubbles 0" or 1). Turning it off clears
// the bubbles showing.
bool speechBubblesShown();
// whether a creature's speech bubble is showing now (a creature shows a
// speech bubble or a thought bubble, not both: owner, 2026-11-13)
bool speechBubbleShowing(Agent* creature);
void setSpeechBubblesShown(bool shown);

// The player's settings file (settings.txt in the user data folder, "key
// value" lines), for any setting.
std::string readPlayerSetting(const std::string& key);
void writePlayerSetting(const std::string& key, const std::string& value);

// Autosave: minutes between automatic saves of the open world (0: only on
// quit and when switching worlds). 5 unless the player chose otherwise
// (owner, 2026-11-11).
int autosaveMinutes();
void setAutosaveMinutes(int minutes);
// the most norns the world allows (the player's setting "population-limit",
// set in the Hatchery; owner, 2026-11-13): C2's egg and hatchery limits
// (EGGL, HATL), which its kiss and egg scripts compare the number of norns
// with; 0 is no limit
int populationLimit();
void setPopulationLimit(int norns);

// Speech History (the toolbar's list): the last things creatures and the
// hand said, newest first.
struct SpokenLine {
	AgentRef speaker; // a creature, or the hand
	std::string text; // as shown
	unsigned int tick = 0; // world tick count
};
const std::deque<SpokenLine>& speechHistory();
void recordSpeech(Agent* speaker, const std::string& text);
// The hand says a line again (Speech History's "say" button): a bubble, and
// creatures in earshot hear it (HEAR_HAND).
void handSays(const std::string& text);
// the last line the hand said (Ctrl+S, as in C2); nothing if none yet
void handSaysAgain();
// The hand has said something: creatures in earshot, or those it names, turn
// to the hand (or to a thing it names that they know the word for), weigh a
// verb they know (HomeostatMind, Params::obey), and the nearest say it back.
void answerHand(const std::string& text);
// what a creature says back, when it is due (called each mind tick)
void tickReply(Agent* creature, oldCreature* c);

// A life stage's voice: pitch (playback rate, 1 = as recorded) and pace
// (syllables per usual time, 1 = as the voice file says).
float voicePitch(int stage);
float voicePace(int stage);

// The creature says the phrase: a speech bubble (the game's own, from the
// player's installed copy) and its voice, pitched and paced for its age.
// Creatures in earshot hear the learned words in it (HEAR_CREATURE).
void creatureSays(Agent* creature, int stage, const Utterance& u);
