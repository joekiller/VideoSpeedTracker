//                  Copyright Paul Reynolds, Locust Avenue, Charlottesville, Va,  2016
//                                     All rights reserved.

//                                     License Agreement
//                                For VideoSpeedTracker (VST)
//                                 (3 - clause BSD License)

// Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following
// conditions are met :

// 1) Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
// 2) Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
//      in the documentation and / or other materials provided with the distribution.
// 3) Neither the name of the copyright holder nor the names of the contributors may be used to endorse or promote products
//    derived from this software without specific prior written permission.

// This software is provided by the copyright holder and contributors “as is” and any express or implied warranties, including,
// but not limited to, the implied warranties of merchantability and fitness for a particular purpose are disclaimed.In no event
// shall copyright holders or contributors be liable for any direct, indirect, incidental, special, exemplary, or consequential
// damages(including, but not limited to, procurement of substitute goods or services; loss of use, data, or profits; or business
// interruption) however caused and on any theory of liability, whether in contract, strict liability, or tort(including negligence
// or otherwise) arising in any way out ofthe use of this software, even if advised of the possibility of such damage.


#include "Globals.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
#include <fstream>
#include <map>
#include <sstream>

using namespace std;

Globals::Globals()
{
}


Globals::~Globals()
{
}

namespace {

string trim(const string& s){
	size_t first = 0;
	while (first < s.size() && isspace((unsigned char)s[first])) first++;
	size_t last = s.size();
	while (last > first && isspace((unsigned char)s[last - 1])) last--;
	return s.substr(first, last - first);
}

string lower(string s){
	for (char& c : s) c = (char)tolower((unsigned char)c);
	return s;
}

// Whole-string integer parse: "35" is fine, "35px", "", "thirty" are not.
bool parseInt(const string& s, int& out){
	try {
		size_t used = 0;
		int v = stoi(s, &used);
		if (trim(s.substr(used)).empty()){ out = v; return true; }
	}
	catch (const exception&) {}
	return false;
}

bool parseDouble(const string& s, double& out){
	try {
		size_t used = 0;
		double v = stod(s, &used);
		if (trim(s.substr(used)).empty()){ out = v; return true; }
	}
	catch (const exception&) {}
	return false;
}

// Strings may be written bare (SE) or quoted ("SE"); the 2016 sample config quotes the compass directions.
string unquote(const string& s){
	if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
	return s;
}

}  // namespace


// Reads VST.cfg.  Format, one setting per line:
//     key = value      # anything after '#' is a comment
// Keys may appear in any order and are matched case-insensitively.  Blank lines, comment-only lines,
// CRLF line endings and a missing or present final newline are all fine.  Keys left out keep the
// defaults in Globals.h.  Unknown keys, unparseable values and an analysis box that doesn't fit the
// 1280x720 frame are reported with the file name and line number, and make this return false.
bool Globals::readConfig(const string& path){

	// key -> setter.  Each setter parses the right-hand side and returns false if it can't.
	auto intKey = [](int& field){ return [&field](const string& v){ return parseInt(v, field); }; };
	auto dblKey = [](double& field){ return [&field](const string& v){ return parseDouble(v, field); }; };
	auto strKey = [](string& field){ return [&field](const string& v){ field = unquote(v); return true; }; };
	const vector<pair<string, function<bool(const string&)>>> keys = {
		{ "dataPathPrefix", strKey(dataPathPrefix) },
		{ "L2RDirection", strKey(L2RDirection) },
		{ "R2LDirection", strKey(R2LDirection) },
		{ "obstruction", [this](const string& v){  // [left,right]; use left >= right (e.g. [0,0]) for no obstruction
			string inner = trim(v);
			if (inner.size() < 2 || inner.front() != '[' || inner.back() != ']') return false;
			inner = inner.substr(1, inner.size() - 2);
			size_t comma = inner.find(',');
			return comma != string::npos && parseInt(inner.substr(0, comma), obstruction[0]) && parseInt(inner.substr(comma + 1), obstruction[1]);
		} },
		{ "AnalysisBoxLeft", intKey(AnalysisBoxLeft) },
		{ "AnalysisBoxTop", intKey(AnalysisBoxTop) },
		{ "AnalysisBoxWidth", intKey(AnalysisBoxWidth) },
		{ "AnalysisBoxHeight", intKey(AnalysisBoxHeight) },
		{ "speedLineLeft", intKey(speedLineLeft) },
		{ "speedLineRight", intKey(speedLineRight) },
		{ "maxL2RDistOnEntry", intKey(maxL2RDistOnEntry) },
		{ "maxR2LDistOnEntry", intKey(maxR2LDistOnEntry) },
		{ "entryLookBack", intKey(entryLookBack) },
		{ "obstruction_extent", intKey(obstruction_extent) },
		{ "largeVehicleArea", intKey(largeVehicleArea) },
		{ "CalibrationFramesL2R", intKey(CalibrationFramesL2R) },
		{ "CalibrationFramesR2L", intKey(CalibrationFramesR2L) },
		{ "CalibrationSpeed", dblKey(CalibrationSpeed) },
		{ "CalibrationFPS", dblKey(CalibrationFPS) },
		{ "SENSITIVITY_VALUE", intKey(SENSITIVITY_VALUE) },
		{ "BLUR_SIZE", intKey(BLUR_SIZE) },
		{ "SLOP", intKey(SLOP) },
		{ "R2LStreetY", intKey(R2LStreetY) },
		{ "L2RStreetY", intKey(L2RStreetY) },
		{ "nextHeight", intKey(nextHeight) },
	};
	map<string, size_t> byName;  // lower-cased key -> index into keys
	for (size_t i = 0; i < keys.size(); i++) byName[lower(keys[i].first)] = i;

	ifstream configIn(path);
	if (!configIn.good()){
		cerr << "Can't open config file " << path << endl;
		return false;
	}

	bool ok = true;
	map<size_t, int> seenOnLine;
	string inLine;
	for (int lineNo = 1; getline(configIn, inLine); lineNo++){
		string text = trim(inLine.substr(0, inLine.find('#')));
		if (text.empty()) continue;  // blank or comment-only line
		auto where = [&](){ return path + ":" + to_string(lineNo) + ": "; };
		size_t eq = text.find('=');
		if (eq == string::npos){
			cerr << where() << "expected \"key = value\", got \"" << text << "\"" << endl;
			ok = false;
			continue;
		}
		string key = trim(text.substr(0, eq));
		string value = trim(text.substr(eq + 1));
		auto found = byName.find(lower(key));
		if (found == byName.end()){
			cerr << where() << "unknown setting \"" << key << "\".  Known settings:";
			for (const auto& k : keys) cerr << " " << k.first;
			cerr << endl;
			ok = false;
			continue;
		}
		const string& name = keys[found->second].first;
		if (seenOnLine.count(found->second))
			cerr << where() << name << " was already set on line " << seenOnLine[found->second] << "; using this later value" << endl;
		seenOnLine[found->second] = lineNo;
		if (!keys[found->second].second(value)){
			cerr << where() << "can't read a value for " << name << " from \"" << value << "\"" << endl;
			ok = false;
			continue;
		}
		cout << name << " = " << value << endl;
	}
	if (!ok) return false;

	// The analysis box, speed lines and hubcap lines are all in 1280x720 frame pixels (frames of other sizes are scaled to it).
	auto bad = [&](const string& why){ cerr << path << ": " << why << endl; ok = false; };
	if (AnalysisBoxLeft < 0 || AnalysisBoxTop < 0 || AnalysisBoxWidth <= 0 || AnalysisBoxHeight <= 0)
		bad("AnalysisBoxLeft/Top must be >= 0 and AnalysisBoxWidth/Height must be > 0");
	if (AnalysisBoxLeft + AnalysisBoxWidth > FrameWidth)
		bad("analysis box too wide: AnalysisBoxLeft + AnalysisBoxWidth = " + to_string(AnalysisBoxLeft + AnalysisBoxWidth) + ", must be <= " + to_string(FrameWidth));
	if (AnalysisBoxTop + AnalysisBoxHeight > FrameHeight)
		bad("analysis box too tall: AnalysisBoxTop + AnalysisBoxHeight = " + to_string(AnalysisBoxTop + AnalysisBoxHeight) + ", must be <= " + to_string(FrameHeight));
	if (speedLineLeft < 0 || speedLineLeft >= speedLineRight || speedLineRight > AnalysisBoxWidth)
		bad("speed lines must satisfy 0 <= speedLineLeft < speedLineRight <= AnalysisBoxWidth (they are relative to AnalysisBoxLeft)");
	if (R2LStreetY <= 0 || R2LStreetY > AnalysisBoxHeight || L2RStreetY <= 0 || L2RStreetY > AnalysisBoxHeight)
		bad("R2LStreetY and L2RStreetY must be between 1 and AnalysisBoxHeight (they are relative to AnalysisBoxTop)");
	if (CalibrationFramesL2R <= 0 || CalibrationFramesR2L <= 0 || CalibrationSpeed <= 0 || CalibrationFPS <= 0)
		bad("CalibrationFramesL2R, CalibrationFramesR2L, CalibrationSpeed and CalibrationFPS must all be > 0");
	if (BLUR_SIZE <= 0)
		bad("BLUR_SIZE must be > 0");
	return ok;
};
