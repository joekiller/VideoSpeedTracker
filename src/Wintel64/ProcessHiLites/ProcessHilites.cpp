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
//

//  Run this code on output highlights file of VideoSpeedTracker.  The program produces highlights of highlights as edited by the user.
// Output is placed in subdirectory "forPosting"  with the prefix "for_post" prepended to the input file name.

// Ported to OpenCV 4 and std::filesystem so it builds on Linux and macOS as well as Windows:
//  - the Windows "dir ... > files.txt" listing is replaced by a directory scan for .avi/.mp4 files
//  - paths are joined with std::filesystem instead of hard-coded backslashes; forPosting is created if missing
//  - the output codec is mp4v (fourcc -1 popped up a codec picker dialog that only exists on Windows)
//  - config errors exit instead of spinning for ctrl-c, and the frame buffer grows instead of overflowing at 200 frames
//  - optional --config / --input / --output arguments skip the config lookup in the working directory and the file prompt

#include <opencv2/opencv.hpp>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <filesystem>

#define CVBlack 0,0,0
#define CVCyan 255,255,153

using namespace std;
using namespace cv;
namespace fs = std::filesystem;

VideoCapture hiLiteVideoIn;  //Input hiLites video
VideoWriter hiLiteVideoOut; // For writing highlights...the edited Scofflaws
vector<Mat> frames;  // Frames of the vehicle clip currently being reviewed; grows as needed
int framePTR = 0;
string fileName;  // Name of video file currently being processed.
int leftSide = 320;  // These values are very specific to the setup used in program trafficReports.   ***********************************
int top = 120;  // It woould be better to have these passed in as parameters from the trafficReports program.   **********************

string inLine, lhs, rhs;
string dataPathPrefix;


bool getSides(string inLine){
	string tempLHS, tempRHS;
	istringstream configLine(inLine);
	getline(configLine, tempLHS, '=');
	getline(configLine, tempRHS, '#');
	lhs = tempLHS;
	rhs = tempRHS;
	return true;
}

// Strips spaces, tabs and the CR of CRLF config files from both ends.
string trim(string toBeTrimmed){
	int first = 0;
	int last = (int)toBeTrimmed.length() - 1;
	while (first <= last && isspace((unsigned char)toBeTrimmed[first])) first++;
	while (last >= first && isspace((unsigned char)toBeTrimmed[last])) last--;
	return toBeTrimmed.substr(first, (last - first) + 1);
}

void replay(int inFramePTR, int inDelay){
	for (int i = 0; i < inFramePTR; i++){
		imshow("Next Frame", frames[i]);
		switch (waitKey(inDelay));
	}
}

bool blackFrame(Mat inFrame){
	string lineIn;
	Scalar res = mean(inFrame);
	return (res[0] < 0.1 && res[1] < 0.1 && res[2] < 0.1);
}

// Reads dataPathPrefix, CropLeft and CropTop, in that order.  Blank lines are skipped.
bool readConfig(const string& path){
	string lhsString[3] = {
		"dataPathPrefix",
		"CropLeft",
		"CropTop"
	};

	ifstream configIn(path);
	if (!configIn.good()){
		cout << "Can't open " << path << "." << endl;
		return false;
	}
	int lineNo = 0;
	while (getline(configIn, inLine)){
		if (trim(inLine).empty()) continue;
		getSides(inLine);
		lhs = trim(lhs);
		rhs = trim(rhs);
		if (lineNo > 2){
			cout << "Too many lines in config file." << endl;
			return false;
		}
		if (lhs != lhsString[lineNo]){
			cout << "Just read LHS doesn't match anything: <" << lhs << ">." << endl;
			return false;
		}
		switch (lineNo){
		case 0:
			dataPathPrefix = rhs;
			cout << "dataPathPrefix = " << dataPathPrefix << endl;
			break;
		case 1:
			leftSide = stoi(rhs);
			cout << "leftSide = " << leftSide << endl;
			break;
		case 2:
			top = stoi(rhs);
			cout << "top = " << top << endl << endl;
			break;
		} // switch
		lineNo++;
	} // while lines remain in config file
	if (lineNo < 3){
		cout << "Config file is missing entries; expected dataPathPrefix, CropLeft, CropTop." << endl;
		return false;
	}
	return true;
}

// Highlights videos in dir, sorted by name (VST writes .avi interactively and whatever --highlights names, usually .mp4).
vector<string> listVideos(const fs::path& dir){
	vector<string> names;
	error_code ec;
	for (const auto& entry : fs::directory_iterator(dir, ec)){
		string ext = entry.path().extension().string();
		transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)tolower(c); });
		if (entry.is_regular_file() && (ext == ".avi" || ext == ".mp4"))
			names.push_back(entry.path().filename().string());
	}
	sort(names.begin(), names.end());
	return names;
}

void printUsage(){
	cout << "Usage: ProcessHiLites [options]" << endl
		<< "  --config PATH   ProcessHiLites.cfg to use [ProcessHiLites.cfg]" << endl
		<< "  --input PATH    highlights video to edit [prompt for one in <dataPathPrefix>/HiLites]" << endl
		<< "  --output PATH   edited video [<input dir>/forPosting/forPost_<input name>]" << endl;
}

int main(int argc, char** argv){
	string configPath = "ProcessHiLites.cfg";
	string inputPath, outputPath;
	for (int i = 1; i < argc; i++){
		string a = argv[i];
		bool hasValue = i + 1 < argc;
		if (a == "--config" && hasValue) configPath = argv[++i];
		else if (a == "--input" && hasValue) inputPath = argv[++i];
		else if (a == "--output" && hasValue) outputPath = argv[++i];
		else if (a == "--help" || a == "-h"){ printUsage(); return 0; }
		else { cout << "Unknown or incomplete option: " << a << endl; printUsage(); return -1; }
	}

// Get data path prefix and crop corner from ProcessHiLites.cfg
	if (!readConfig(configPath)) return -1;

	fs::path dirPath;
	if (!inputPath.empty()){
		dirPath = fs::path(inputPath).parent_path();
		fileName = fs::path(inputPath).filename().string();
	}
	else {
		dirPath = fs::path(dataPathPrefix) / "HiLites";
		vector<string> files = listVideos(dirPath);
		if (files.empty()){
			cout << "No .avi or .mp4 files in " << dirPath.string() << endl;
			return -1;
		}
		string yesNo = "n";
		// Get file user wants
		while (yesNo == "n"){
			for (const string& name : files){
				fileName = name;
				cout << "Want the file " << fileName << "  (y/n) [n]: ";
				if (!getline(cin, yesNo)) return -1;
				if (yesNo == "y") break;  // User has chosen the file
				else yesNo = "n";
			}
		}
	}

	string fullName = (dirPath / fileName).string();
	cout << "Fullname is: <" << fullName << "> " << endl;
	hiLiteVideoIn.open(fullName);

	if (!hiLiteVideoIn.isOpened()){
		cout << "ERROR ACQUIRING VIDEO FEED\n";
		getchar();
		return 0;
	}

	frames.resize(1);
	if (hiLiteVideoIn.read(frames[0])){
		if (leftSide < 0 || top < 0 || leftSide + 640 > frames[0].cols || top + 360 > frames[0].rows){
			cout << "Crop 640x360 at (" << leftSide << ", " << top << ") doesn't fit in the " << frames[0].cols << "x" << frames[0].rows
				<< " input.  Fix CropLeft/CropTop in " << configPath << endl;
			return -1;
		}
// NOTE: the following operation crops a 640 x 360 rectangle out of the middlle of input frame.  Dependng on the width of your
// speed zone, this may not work for you...parts of your speed zone may be cropped off the ends.
		rectangle(frames[0], Rect(Point(leftSide, top), Point(leftSide + 639, top + 359)), Scalar(CVCyan), 4);
		imshow("Next Frame", frames[0]);
		switch (waitKey(20));
		string yesNo = "n";
		cout << "Is this cropping OK for saved captures? (y/n) [y]: ";
		getline(cin, yesNo);
		if (yesNo == "n") return -1; // Coould modify this to move cropping rectangle around until user happy.
	}

	hiLiteVideoIn.release();
	switch (waitKey(20));
	hiLiteVideoIn.open(fullName);
	if (!hiLiteVideoIn.isOpened()){
		cout << "ERROR ACQUIRING VIDEO FEED\n";
		getchar();
		return 0;
	}

	if (outputPath.empty()){
		error_code ec;
		fs::create_directories(dirPath / "forPosting", ec);
		outputPath = (dirPath / "forPosting" / ("forPost_" + fileName)).string();
	}
	cout << "Writing " << outputPath << endl;
	hiLiteVideoOut.open(outputPath, VideoWriter::fourcc('m', 'p', '4', 'v'), hiLiteVideoIn.get(CAP_PROP_FPS), Size(640, 360), true);
	if (!hiLiteVideoOut.isOpened()){
		cout << "ERROR Opening output file\n";
		getchar();
		return 0;
	}


	bool atVideoEnd = false;
	string response;

	while (!atVideoEnd && hiLiteVideoIn.get(CAP_PROP_POS_FRAMES) < hiLiteVideoIn.get(CAP_PROP_FRAME_COUNT)){

		framePTR = 0;
		bool atVehicleClipEnd = false;
		while (!atVehicleClipEnd){
			if ((int)frames.size() <= framePTR) frames.resize(framePTR + 1);
			if (hiLiteVideoIn.read(frames[framePTR])){
				if (!blackFrame(frames[framePTR])){  //  If this is a black frame, keep it but don't show it
					imshow("Next Frame", frames[framePTR]);
					switch (waitKey(20));
				}
				else {
					atVehicleClipEnd = true;
				}
				framePTR++;
			}
			else{
				atVehicleClipEnd = true;
				atVideoEnd = true;
			}
		} // while reading frames for current vehicle


		if (!atVideoEnd){
			bool noAction = true;
			while (noAction){
				cout << "[K]eep vehicle,  [D]elete vehicle,  [R]eplay,  [S]low replay,  [Q]uit: ";
				if (!getline(cin, response)) response = "q";  // stdin closed: finish up as if the user quit
				if (response == "d"){  // "d"   delete up to and including last frame read.
					framePTR = 0;
					noAction = false;
				}
				else if (response == "k"){  // "k" keep vehicle in hiLites
					Rect crop(leftSide, top, 640, 360);
					for (int i = 0; i < 15; i++)
						hiLiteVideoOut.write(frames[0](crop));
					for (int i = 0; i < framePTR-1; i++)
						hiLiteVideoOut.write(frames[i](crop));
					for (int i = 0; i < 20; i++)
						hiLiteVideoOut.write(frames[max(framePTR - 2, 0)](crop));
					hiLiteVideoOut.write(frames[framePTR - 1](crop));
					framePTR = 0;
					noAction = false;
				}
				else if (response == "r"){  // "r"  replay normal speed
					replay(framePTR-1, 20);
				}
				else if (response == "s"){  // "s"  replay slow speed
					replay(framePTR-1, 250);
				}
				else if (response == "q"){  // "q"  quit.
					cout << "quitting" << endl;
					noAction = false;
				}
			} // while no action
		}  // If not at video end

		if (response == "q")
			break;          //  Stop reading any frames and finish up.
	} // while frames remain to be read

	if (framePTR > 0){
		cout << "There are " << framePTR << " frames left in buffer.  Write them out? [y|n] <y>: ";
		getline(cin, response);
		if (!response.empty() && response == "y")
			for (int i = 0; i < framePTR; i++)
				hiLiteVideoOut.write(frames[i](Rect(leftSide, top, 640, 360)));
	}

	hiLiteVideoIn.release();
	hiLiteVideoOut.release();

	return 0;
}
