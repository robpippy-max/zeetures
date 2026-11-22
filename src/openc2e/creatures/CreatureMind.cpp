/*
 *  CreatureMind.cpp
 *  openc2e
 *
 *  Mind registry and the "null" mind.
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

#include "CreatureMind.h"

std::unique_ptr<CreatureMind> createHomeostatMind(); // HomeostatMind.cpp
std::unique_ptr<CreatureMind> createLlmMind(); // LlmMind.cpp

std::unique_ptr<CreatureMind> createCreatureMind(const std::string& name) {
	if (name == "homeostat")
		return createHomeostatMind();
	if (name == "llm")
		return createLlmMind();
	return nullptr;
}

std::string availableCreatureMinds() {
	return "homeostat, llm";
}
