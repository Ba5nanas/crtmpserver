/* 
 *  Copyright (c) 2010,
 *  Gavriloaie Eugen-Andrei (shiretu@gmail.com)
 *
 *  This file is part of crtmpserver.
 *  crtmpserver is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  crtmpserver is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with crtmpserver.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAS_MEDIA_FLV
#include "utils/file.h"
#include "mediaformats/flv/flvdocument.h"
#include "buffering/iobuffer.h"
#include "protocols/rtmp/amf0serializer.h"

FLVDocument::FLVDocument(Variant &metadata)
: BaseMediaDocument(metadata) {

}

FLVDocument::~FLVDocument() {

}

bool FLVDocument::ParseDocument() {
	return true;
}

bool FLVDocument::BuildFrames() {
	vector<MediaFrame> binaryHeaders;
	//1. Go to the beginning of the file
	if (!_mediaFile.SeekBegin()) {
		FATAL("Unable to seek in file");
		return false;
	}

	//2. Ignore the flv header
	if (!_mediaFile.SeekAhead(9)) {
		FATAL("Unable to seek in file");
		return false;
	}

	//3. We are not interested in the previous tag size
	if (!_mediaFile.SeekAhead(4)) {
		FATAL("Unable to seek in file");
		return false;
	}

	//4. Build the frames
	MediaFrame frame = {0};
	uint8_t tagType = 0;
	uint32_t timestamp;
	while (_mediaFile.Cursor() != _mediaFile.Size()) {
		//5. Read the tag type
		if (!_mediaFile.ReadUI8(&tagType)) {
			WARN("Unable to read data");
			break;
		}

		//6. Set the frame type based on the tag type
		//Also set the iskeyFrame property here
		switch (tagType) {
			case 8: //audio data
				_audioSamplesCount++;
				frame.type = MEDIAFRAME_TYPE_AUDIO;
				break;
			case 9: //video data
				_videoSamplesCount++;
				frame.type = MEDIAFRAME_TYPE_VIDEO;
				break;
			case 18: //info data
				frame.type = MEDIAFRAME_TYPE_DATA;
				break;
			default:
				WARN("Invalid tag type: %d at cursor %llu", tagType, _mediaFile.Cursor());
				return true;
		}

		//7. Read the frame length
		uint32_t tempLength;
		if (!_mediaFile.ReadUI24(&tempLength)) {
			WARN("Unable to read data");
			break;
		}
		frame.length = tempLength;

		//8. read the timestamp and set the timing on the frame
		if (!_mediaFile.ReadSUI32(&timestamp)) {
			WARN("Unable to read data");
			break;
		}
		//TODO: correctly compute delta time
		frame.deltaTime = 0;
		frame.absoluteTime = timestamp;

		//9. Ignore the stream ID
		if (!_mediaFile.SeekAhead(3)) {
			WARN("Unable to seek in file");
			break;
		}

		//10. Save the start of the data
		frame.start = _mediaFile.Cursor();

		//11. Set the isKeyFrame flag and the isBinary flag
		if (frame.type == MEDIAFRAME_TYPE_VIDEO) {
			uint8_t byte = 0;
			if (!_mediaFile.PeekUI8(&byte)) {
				FATAL("Unable to peek byte");
				return false;
			}
			frame.isKeyFrame = ((byte >> 4) == 1);
			if (frame.isKeyFrame) {
				frame.isBinaryHeader = ((byte & 0x0f) == 7);
				if (frame.isBinaryHeader) {
					uint64_t dword;
					if (!_mediaFile.PeekUI64(&dword)) {
						FATAL("Unable to peek byte");
						return false;
					}
					frame.isBinaryHeader = (((dword >> 48)&0xff) == 0);
				}
			} else {
				frame.isBinaryHeader = false;
			}
		} else {
			frame.isKeyFrame = true;
			uint8_t byte = 0;
			if (!_mediaFile.PeekUI8(&byte)) {
				FATAL("Unable to peek byte");
				return false;
			}
			frame.isBinaryHeader = ((byte >> 4) == 10);
			if (frame.isBinaryHeader) {
				uint16_t word;
				if (!_mediaFile.PeekUI16(&word)) {
					FATAL("Unable to peek byte");
					return false;
				}
				frame.isBinaryHeader = ((word & 0x00ff) == 0);
			}
		}
		if (frame.isBinaryHeader)
			WARN("frame: %s", STR(frame));

		//12. Read the metadata or ignore the data payload
		if (frame.type == MEDIAFRAME_TYPE_DATA) {
			IOBuffer tempBuffer;
			tempBuffer.ReadFromFs(_mediaFile, frame.length);
			//tempBuffer.PutInputBuffer(&buffer, 0, length);
#ifdef HAS_PROTOCOL_RTMP
			AMF0Serializer amfSerializer;

			string name = "";
			Variant parameters;

			Variant tempVariant;
			if (!amfSerializer.Read(tempBuffer, tempVariant)) {
				FATAL("Unable to read metadata");
				return false;
			}
			if (tempVariant != V_STRING) {
				FATAL("Unable to read metadata");
				return false;
			}
			name = ((string) tempVariant);

			while (GETAVAILABLEBYTESCOUNT(tempBuffer) > 0) {
				tempVariant.Reset();
				if (!amfSerializer.Read(tempBuffer, tempVariant)) {
					FATAL("Unable to read metadata");
					return false;
				}
				parameters.PushToArray(tempVariant);
			}

			//            FINEST("name:\n%s", STR(name));
			//            FINEST("parameters:\n%s", STR(parameters.ToString()));
			if (_metadata == V_NULL)
				_metadata = parameters[(uint32_t) 0];
#endif
		} else {
			if (!_mediaFile.SeekAhead(frame.length)) {
				WARN("Unable to seek in file");
				break;
			}
		}

		//13. We are not interested in the previous tag size
		if (!_mediaFile.SeekAhead(4)) {
			WARN("Unable to seek in file");
			break;
		}

		//14. Store it in the proper location and adjust the timestamp accordingly
		if (frame.isBinaryHeader) {
			frame.absoluteTime = 0;
			ADD_VECTOR_END(binaryHeaders, frame);
		} else {
			ADD_VECTOR_END(_frames, frame);
		}
	}

	//    for (uint32_t i = 0; i < 50; i++) {
	//        FINEST("Before:%s", STR(_frames[i]));
	//    }
	sort(_frames.begin(), _frames.end(), CompareFrames);
	//    for (uint32_t i = 0; i < _frames.size(); i++) {
	//        FINEST("After:%s", STR(_frames[i]));
	//    }
	//    NYIR;

	//15. Add the binary headers
	for (uint32_t i = 0; i < binaryHeaders.size(); i++) {
		ADD_VECTOR_BEGIN(_frames, binaryHeaders[i]);
	}

	return true;
}

Variant FLVDocument::GetRTMPMeta() {
	return _metadata;
}

#endif /* HAS_MEDIA_FLV */