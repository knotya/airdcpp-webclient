/*
 * Copyright (C) 2011 AirDC++ Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "stdinc.h"
#include <direct.h>
#include "TargetUtil.h"
#include "ShareManager.h"

#include "QueueManager.h"


#include <boost/range/algorithm/for_each.hpp>
#include <boost/range/algorithm_ext/for_each.hpp>
#include <boost/range/adaptor/map.hpp>
//#include <boost/range/algorithm/sort.hpp>

#ifdef _WIN32
# include <ShlObj.h>
#else
# include <dirent.h>
# include <sys/stat.h>
# include <unistd.h>
# include <fnmatch.h>
#endif

namespace dcpp {

using boost::range::for_each;
using boost::adaptors::map_values;

string TargetUtil::getMountPath(const string& aPath) {
	TCHAR buf[MAX_PATH];
	TCHAR buf2[MAX_PATH];
	string::size_type l = aPath.length();
	for (;;) {
		l = aPath.rfind('\\', l-2);
		if (l == string::npos || l <= 1)
			break;
		if (GetVolumeNameForVolumeMountPoint(Text::toT(aPath.substr(0, l+1)).c_str(), buf, MAX_PATH) && GetVolumePathNamesForVolumeName(buf, buf2, MAX_PATH, NULL)) {
			return Text::fromT(buf2);
		}
	}
	return Util::emptyString;
}

string TargetUtil::getMountPath(const string& aPath, const StringSet& aVolumes) {
	if (aVolumes.find(aPath) != aVolumes.end()) {
		return aPath;
	}
	string::size_type l = aPath.length();
	for (;;) {
		l = aPath.rfind('\\', l-2);
		if (l == string::npos || l <= 1)
			break;
		if (aVolumes.find(aPath.substr(0, l+1)) != aVolumes.end()) {
			return aPath.substr(0, l+1);
		}
	}

	//not found from the volumes... network path? this won't work with mounted dirs
	if (aPath.length() > 2 && aPath.substr(0,2) == "\\\\") {
		l = aPath.find("\\", 2);
		if (l != string::npos) {
			//get the drive letter
			l = aPath.find("\\", l+1);
			if (l != string::npos) {
				return aPath.substr(0, l+1);
			}
		}
	}
	return Util::emptyString;
}

bool TargetUtil::getVirtualTarget(const string& aTarget, TargetUtil::TargetType targetType, TargetInfo& ti_, const int64_t& aSize) {
	if (targetType == TARGET_PATH) {
		ti_.targetDir = aTarget;
	} else {
		vector<pair<string, StringList>> dirList;
		if (targetType == TARGET_FAVORITE) {
			dirList = FavoriteManager::getInstance()->getFavoriteDirs();
		} else {
			ShareManager::getInstance()->LockRead();
			dirList = ShareManager::getInstance()->getGroupedDirectories();
			ShareManager::getInstance()->unLockRead();
		}

		auto s = find_if(dirList.begin(), dirList.end(), CompareFirst<string, StringList>(aTarget));
		if (s != dirList.end()) {
			StringList& targets = s->second;
			bool tmp = getTarget(targets, ti_, aSize);
			if (!ti_.targetDir.empty()) {
				return tmp;
			}
		}
	}

	if (ti_.targetDir.empty()) {
		//failed to get the target, use the default one
		ti_.targetDir = SETTING(DOWNLOAD_DIRECTORY);
	}
	return getDiskInfo(ti_);
}

bool TargetUtil::getTarget(StringList& targets, TargetInfo& retTi_, const int64_t& aSize) {
	StringSet volumes;
	getVolumes(volumes);
	TargetInfoMap targetMap;
	int64_t tmpSize = 0;

	for(auto i = targets.begin(); i != targets.end(); ++i) {
		string target = getMountPath(*i, volumes);
		if (!target.empty() && targetMap.find(target) == targetMap.end()) {
			int64_t free = 0;
			if (GetDiskFreeSpaceEx(Text::toT(target).c_str(), NULL, (PULARGE_INTEGER)&tmpSize, (PULARGE_INTEGER)&free)) {
				targetMap[target] = TargetInfo(*i, free);
			}
		}
	}

	if (targetMap.empty()) {
		//failed to get the volumes
		if (!targets.empty()) {
			retTi_.targetDir = targets.front();
		} else {
			retTi_.targetDir = SETTING(DOWNLOAD_DIRECTORY);
		}

		GetDiskFreeSpaceEx(Text::toT(retTi_.targetDir).c_str(), NULL, (PULARGE_INTEGER)&tmpSize, (PULARGE_INTEGER)&retTi_.diskSpace);
		return retTi_.getFreeSpace() >= aSize;
	}

	QueueManager::getInstance()->getDiskInfo(targetMap, volumes);

	compareMap(targetMap, retTi_, aSize, SETTING(DL_AUTOSELECT_METHOD));
	if (retTi_.targetDir.empty()) //no dir with enough space, choose the one with most space available
		compareMap(targetMap, retTi_, aSize, (int8_t)SettingsManager::SELECT_MOST_SPACE);

	return retTi_.getFreeSpace() >= aSize;
}

void TargetUtil::compareMap(const TargetInfoMap& aTargetMap, TargetInfo& retTi_, const int64_t& aSize, int8_t aMethod) {

	for (auto i = aTargetMap.begin(); i != aTargetMap.end(); ++i) {
		auto mapTi = i->second;
		if (aMethod == (int8_t)SettingsManager::SELECT_LEAST_SPACE) {
			int64_t diff = mapTi.getFreeSpace() - aSize;
			if (diff > 0 && (diff < (retTi_.getFreeSpace() - aSize) || !retTi_.isInitialized()))
				retTi_ = mapTi;
		} else if (mapTi.getFreeSpace() > retTi_.getFreeSpace() || !retTi_.isInitialized()) {
			retTi_ = mapTi;
		}
	}
}

bool TargetUtil::getDiskInfo(TargetInfo& targetInfo_) {
	StringSet volumes;
	getVolumes(volumes);
	string pathVol = getMountPath(targetInfo_.targetDir, volumes);
	if (pathVol.empty()) {
		return false;
	}

	int64_t totalSize = 0;
	GetDiskFreeSpaceEx(Text::toT(pathVol).c_str(), NULL, (PULARGE_INTEGER)&totalSize, (PULARGE_INTEGER)&targetInfo_.diskSpace);

	TargetInfoMap targetMap;
	targetMap[pathVol] = targetInfo_;

	QueueManager::getInstance()->getDiskInfo(targetMap, volumes);
	targetInfo_ = targetMap[pathVol];
	return true;
}

void TargetUtil::getVolumes(StringSet& volumes) {
	TCHAR   buf[MAX_PATH];  
	HANDLE  hVol;    
	BOOL    found;
	TCHAR   buf2[MAX_PATH];

	// lookup drive volumes.
	hVol = FindFirstVolume(buf, MAX_PATH);
	if(hVol != INVALID_HANDLE_VALUE) {
		found = true;
		//while we find drive volumes.
		while(found) {
			if(GetDriveType(buf) != DRIVE_CDROM && GetVolumePathNamesForVolumeName(buf, buf2, MAX_PATH, NULL)) {
				volumes.insert(Text::fromT(buf2));
			}
			found = FindNextVolume( hVol, buf, MAX_PATH );
		}
   		found = FindVolumeClose(hVol);
	}

	// and a check for mounted Network drives, todo fix a better way for network space
	ULONG drives = _getdrives();
	TCHAR drive[3] = { _T('A'), _T(':'), _T('\0') };

	while(drives != 0) {
		if(drives & 1 && ( GetDriveType(drive) != DRIVE_CDROM && GetDriveType(drive) == DRIVE_REMOTE) ){
			string path = Text::fromT(drive);
			if( path[ path.length() -1 ] != PATH_SEPARATOR ) {
				path += PATH_SEPARATOR;
			}
			volumes.insert(path);
		}

		++drive[0];
		drives = (drives >> 1);
	}
}

} //dcpp