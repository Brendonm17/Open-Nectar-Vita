#include "kh/khDayEndResult.h"

namespace kh {
namespace Screen {
/**
 * @note Address: 0x80410FE0
 * @note Size: 0x9C
 */
void MailSaveData::clear()
{
	mPastLogs.clear();

	for (int i = 0; i < 20; i++) {
		mHistory.byteView[i] = -1;
	}
}

/**
 * @note Address: 0x8041107C
 * @note Size: 0x88
 */
void MailSaveData::read(Stream& stream)
{
	mPastLogs.readBytes(stream);
	for (s32 i = 0; i < 20; i++) {
		mHistory.byteView[i] = stream.readByte();
	}
}

/**
 * @note Address: 0x80411104
 * @note Size: 0x88
 */
void MailSaveData::write(Stream& stream)
{
	mPastLogs.writeBytes(stream);

	for (s32 i = 0; i < 0x14; i++) {
		stream.writeByte(mHistory.byteView[i]);
	}
}

/**
 * @note Address: 0x8041118C
 * @note Size: 0x28
 */
void MailSaveData::set_history(s8 history)
{
	// Original: mPastLogs.byteView[i + 16] = [i + 15], que se sale de sus 16
	// bytes para caer en mHistory (justo detrás). Lo mismo, sin salirse.
	for (int i = 19; i; i--) {
		mHistory.byteView[i] = mHistory.byteView[i - 1];
	}
	mHistory.byteView[0] = history;
}
} // namespace Screen
} // namespace kh
