/*
* Copyright (C) 2011-2019 AirDC++ Project
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

#include <web-server/FileServer.h>
#include <web-server/WebServerManager.h>
#include <web-server/WebUserManager.h>

#include <api/common/Deserializer.h>

#include <airdcpp/AirUtil.h>
#include <airdcpp/File.h>
#include <airdcpp/Util.h>

#include <airdcpp/ViewFileManager.h>

#include <sstream>

namespace webserver {
	using namespace dcpp;

	FileServer::FileServer() {
	}

	FileServer::~FileServer() {
		RLock l(cs);
		for (const auto& f: tempFiles) {
			File::deleteFile(f.second);
		}
	}

	const string& FileServer::getResourcePath() const noexcept {
		return resourcePath;
	}

	void FileServer::setResourcePath(const string& aPath) noexcept {
		resourcePath = Util::validatePath(aPath, true);
	}

	struct mime { const char* ext; const char* type; };
	struct mime mimes[] = {
		{ "exe", "application/octet-stream" },
		{ "pdf", "application/pdf" },
		{ "zip", "application/zip" },
		{ "gz", "application/x-gzip" },
		{ "js", "application/javascript; charset=utf-8" },

		{ "flac", "audio/x-flac" },
		{ "m4a", "audio/mp4" },
		{ "mid", "audio/midi" },
		{ "mp3", "audio/mpeg" },
		{ "ogg", "audio/ogg" },
		{ "wma", "audio/x-ms-wma" },
		{ "wav", "audio/vnd.wave" },

		{ "bmp", "image/bmp" },
		{ "gif", "image/gif" },
		{ "ico", "image/x-icon" },
		{ "jpg", "image/jpeg" },
		{ "jpeg", "image/jpeg" },
		{ "png", "image/png" },
		{ "psd", "image/vnd.adobe.photoshop" },
		{ "tga", "image/tga" },
		{ "tiff", "image/tiff" },
		{ "tif", "image/tiff" },
		{ "ico", "image/vnd.microsoft.icon" },
		{ "webp", "image/webp" },

		{ "3gp", "video/3gpp" },
		{ "avi", "video/avi" },
		{ "asf", "video/x-ms-asf" },
		{ "asx", "video/x-ms-asf" },
		{ "flv", "video/x-flv" },
		{ "mkv", "video/x-matroska" },
		{ "mov", "video/quicktime" },
		{ "mpg", "video/mpeg" },
		{ "mpeg", "video/mpeg" },
		{ "mp4", "video/mp4" },
		{ "qt", "video/quicktime" },
		{ "webm", "video/webm" },
		{ "wmv", "video/x-ms-wmv" },
		{ "vob", "video/x-ms-vob" },

		{ "odt", "application/vnd.oasis.opendocument.text" },
		{ "ods", "application/vnd.oasis.opendocument.spreadsheet" },
		{ "odp", "application/vnd.oasis.opendocument.presentation" },
		{ "odg", "application/vnd.oasis.opendocument.graphics" },
		{ "xls", "application/vnd.ms-excel" },
		{ "ppt", "application/vnd.ms-powerpoint" },
		{ "doc", "application/msword" },
		{ "docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
		{ "ttf", "application/x-font-ttf" },
		{ "rar", "application/x-rar-compressed" },
		{ "tar", "application/x-tar" },
		{ "swf", "application/x-shockwave-flash" },

		{ "c", "text/plain" },
		{ "cpp", "text/plain" },
		{ "asm", "text/plain" },
		{ "bat", "text/plain" },
		{ "vb", "text/plain" },
		{ "cs", "text/plain" },
		{ "nfo", "text/x-nfo" },
		{ "pl", "text/plain" },
		{ "py", "text/plain" },
		{ "class", "text/plain" },
		{ "vbs", "text/plain" },
		{ "css", "text/css" },
		{ "html", "text/html; charset=utf-8" },
		{ "txt", "text/plain" },
		{ "xml", "text/xml" },
		{ NULL, NULL }
	};

	const char* FileServer::getMimeType(const string& aFileName) noexcept {
		auto extension = getExtension(aFileName);
		for (int i = 0; mimes[i].ext != NULL; i++) {
			if (extension == mimes[i].ext) {
				return mimes[i].type;
			}
		}

		return nullptr;
	}

	string FileServer::getExtension(const string& aResource) noexcept {
		auto extension = Util::getFileExt(aResource);
		if (!extension.empty()) {
			// Strip the dot
			extension = extension.substr(1);
		}

		return extension;
	}

	void FileServer::addCacheControlHeader(StringPairList& headers_, int aDaysValid) noexcept {
		headers_.emplace_back("Cache-Control", aDaysValid == 0 ? "no-store" : "max-age=" + Util::toString(aDaysValid * 24 * 60 * 60));
	}

	string FileServer::parseResourcePath(const string& aResource, const websocketpp::http::parser::request& aRequest, StringPairList& headers_) const {
		// Serve files only from the resource directory
		if (aResource.empty() || aResource.find("..") != std::string::npos) {
			throw RequestException(websocketpp::http::status_code::bad_request, "Invalid resource path");
		}

		auto request = aResource;

		auto extension = getExtension(request);
		if (!extension.empty()) {
			dcassert(extension[0] != '.');

			// We have compressed versions only for JS files
			if (extension == "js" && aRequest.get_header("Accept-Encoding").find("gzip") != string::npos) {
				request += ".gz";
				// The Content-Encoding header will be set only after the file has been read successfully
				// as gzip encoding shouldn't be used in case of errors...
			}

			if (extension != "html" && aResource != "/sw.js") {
				// File versioning is done with hashes in filenames (except for the index file and service worker)
				addCacheControlHeader(headers_, 365);
			}
		} else {
			// Forward all requests for non-static files to index
			// (but try to report API requests or other downloads with an invalid path)

			if (aRequest.get_header("Accept").find("text/html") == string::npos) {
				if (aRequest.get_header("Content-Type") == "application/json") {
					throw RequestException(websocketpp::http::status_code::not_acceptable, "File server won't serve JSON files. Did you mean \"/api" + aResource + "\" instead?");
				}

				throw RequestException(websocketpp::http::status_code::not_found, "Invalid file path (hint: use \"Accept: text/html\" if you want index.html)");
			}

			request = "index.html";

			// The main chunk name may change and it's stored in the HTML file
			addCacheControlHeader(headers_, 0);
		}

		// Avoid double separators because of assertions
		if (!request.empty() && request.front() == '/') {
			request = request.substr(1);
		}

		// For windows
		Util::replace(request, "/", PATH_SEPARATOR_STR);

		return resourcePath + request;
	}

	string FileServer::parseViewFilePath(const string& aResource, StringPairList& headers_, const SessionPtr& aSession) const {
		string protocol, tthStr, port, path, query, fragment;
		Util::decodeUrl(aResource, protocol, tthStr, port, path, query, fragment);

		auto session = aSession;
		if (!session) {
			auto auth = Util::decodeQuery(query)["auth_token"];
			if (!auth.empty()) {
				session = WebServerManager::getInstance()->getUserManager().getSession(auth);
			}

			if (!session || !session->getUser()->hasPermission(Access::VIEW_FILES_VIEW)) {
				throw RequestException(websocketpp::http::status_code::unauthorized, "Not authorized");
			}
		}

		auto tth = Deserializer::parseTTH(tthStr);
		auto paths = AirUtil::getFileDupePaths(AirUtil::checkFileDupe(tth), tth);
		if (paths.empty()) {
			auto file = ViewFileManager::getInstance()->getFile(tth);
			if (!file) {
				throw RequestException(websocketpp::http::status_code::not_found, "No files matching the TTH were found");
			}

			paths.push_back(file->getPath());
		}

		addCacheControlHeader(headers_, 1); // One day (files are identified by their TTH so the content won't change)

		return paths.front();
	}

	string FileServer::formatPartialRange(int64_t aStartPos, int64_t aEndPos, int64_t aFileSize) noexcept {
		dcassert(aEndPos < aFileSize);
		return "bytes " + Util::toString(aStartPos) + "-" + Util::toString(aEndPos) + "/" + Util::toString(aFileSize);
	}

	// Support partial requests will enhance media file playback
	// This will only support simple range values (unsupported range types will be ignored)
	bool FileServer::parsePartialRange(const string& aHeaderData, int64_t& start_, int64_t& end_) noexcept {
		if (aHeaderData.find("bytes=") != 0) {
			return false;
		}

		dcdebug("Partial HTTP request: %s)\n", aHeaderData.c_str());

		auto tokenizer = StringTokenizer<string>(aHeaderData.substr(6), '-', true);
		if (tokenizer.getTokens().size() != 2) {
			dcdebug("Partial HTTP request: unsupported range\n");
			return false;
		}

		auto parsedStart = Util::toInt64(tokenizer.getTokens().at(0));

		// Not "parsedStart >= end_" because Safari seems to request one byte past the end (shouldn't be an issue when reading the file)
		if (parsedStart > end_ || parsedStart < 0) {
			dcdebug("Partial HTTP request: start position not accepted (" I64_FMT ")\n", parsedStart);
			return false;
		}

		const auto& endToken = tokenizer.getTokens().at(1);
		if (endToken.empty()) {
			end_ = end_ - start_;
		} else {
			auto parsedEnd = Util::toInt64(endToken);
			if (parsedEnd > end_ || parsedEnd <= parsedStart) {
				dcdebug("Partial HTTP request: end position not accepted (parsed start: " I64_FMT ", parsed end: " I64_FMT ", file size: " I64_FMT ")\n", parsedStart, parsedEnd, end_);
				return false;
			}

			end_ = parsedEnd;
		}

		// Both values were passed successfully
		start_ = parsedStart;
		return true;
	}


	websocketpp::http::status_code::value FileServer::handlePostRequest(const websocketpp::http::parser::request& aRequest,
		std::string& output_, StringPairList& headers_, const SessionPtr& aSession) noexcept {

		const auto& requestPath = aRequest.get_uri();
		if (requestPath == "/temp") {
			if (!aSession || !aSession->getUser()->hasPermission(Access::FILESYSTEM_EDIT)) {
				output_ = "Not authorized";
				return websocketpp::http::status_code::unauthorized;
			}

			const auto fileName = Util::toString(Util::rand());
			const auto filePath = Util::getTempPath() + fileName;

			try {
				File file(filePath, File::WRITE, File::TRUNCATE | File::CREATE, File::BUFFER_SEQUENTIAL);
				file.write(aRequest.get_body());
			} catch (const FileException& e) {
				output_ = "Failed to write the file: " + e.getError();
				return websocketpp::http::status_code::internal_server_error;
			}

			{
				WLock l(cs);
				tempFiles.emplace(fileName, filePath);
			}

			headers_.emplace_back("Location", fileName);
			return websocketpp::http::status_code::created;
		}

		output_ = "Requested resource was not found";
		return websocketpp::http::status_code::not_found;
	}

	string FileServer::getTempFilePath(const string& fileId) const noexcept {
		RLock l(cs);
		auto i = tempFiles.find(fileId);
		return i != tempFiles.end() ? i->second : Util::emptyString;
	}

	websocketpp::http::status_code::value FileServer::handleRequest(const websocketpp::http::parser::request& aRequest,
		string& output_, StringPairList& headers_, const SessionPtr& aSession) noexcept {

		if (aRequest.get_method() == "GET") {
			return handleGetRequest(aRequest, output_, headers_, aSession);
		} else if (aRequest.get_method() == "POST") {
			return handlePostRequest(aRequest, output_, headers_, aSession);
		}

		output_ = "Requested resource was not found";
		return websocketpp::http::status_code::not_found;
	}

	websocketpp::http::status_code::value FileServer::handleGetRequest(const websocketpp::http::parser::request& aRequest,
		string& output_, StringPairList& headers_, const SessionPtr& aSession) noexcept {

		const auto& requestUrl = aRequest.get_uri();
		dcdebug("Requesting file %s\n", requestUrl.c_str());

		// Get the disk path
		string filePath;
		try {
			if (requestUrl.length() >= 6 && requestUrl.compare(0, 6, "/view/") == 0) {
				filePath = parseViewFilePath(requestUrl.substr(6), headers_, aSession);
			} else {
				filePath = parseResourcePath(requestUrl, aRequest, headers_);
			}
		} catch (const RequestException& e) {
			output_ = e.what();
			return e.getCode();
		}

		auto fileSize = File::getSize(filePath);
		int64_t startPos = 0, endPos = fileSize - 1;

		auto partialContent = parsePartialRange(aRequest.get_header("Range"), startPos, endPos);

		// Read file
		try {
			File f(filePath, File::READ, File::OPEN);
			f.setPos(startPos);
			output_ = f.read(static_cast<size_t>(endPos) + 1);
		} catch (const FileException& e) {
			dcdebug("Failed to serve the file %s: %s\n", filePath.c_str(), e.getError().c_str());
			output_ = e.getError();
			return websocketpp::http::status_code::not_found;
		} catch (const std::bad_alloc&) {
			output_ = "Not enough memory on the server to serve this request";
			return websocketpp::http::status_code::internal_server_error;
		}

		{
			const auto ext = Util::getFileExt(filePath);
			if (ext == ".nfo") {
				string encoding;

				// Platform-independent encoding conversion function could be added if there is more use for it
#ifdef _WIN32
				encoding = "CP.437";
#else
				encoding = "cp437";
#endif
				output_ = Text::toUtf8(output_, encoding);
			} else if (ext == ".gz" && aRequest.get_header("Accept-Encoding").find("gzip") != string::npos) {
				headers_.emplace_back("Content-Encoding", "gzip");
			}
		}

		{
			// Get the mime type (but get it from the original request with gzipped content)
			auto usingEncoding = find_if(headers_.begin(), headers_.end(), CompareFirst<string, string>("Content-Encoding")) != headers_.end();
			auto type = getMimeType(usingEncoding ? requestUrl : filePath);
			if (type) {
				headers_.emplace_back("Content-Type", type);
			}
		}

		if (partialContent) {
			headers_.emplace_back("Content-Range", formatPartialRange(startPos, endPos, fileSize));
			headers_.emplace_back("Accept-Ranges", "bytes");
			return websocketpp::http::status_code::partial_content;
		}

		return websocketpp::http::status_code::ok;
	}
}