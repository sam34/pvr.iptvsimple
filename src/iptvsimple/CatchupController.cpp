/*
 *      Copyright (C) 2005-2019 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1335, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "CatchupController.h"

#include "../client.h"
#include "Channels.h"
#include "Epg.h"
#include "Settings.h"
#include "data/Channel.h"
#include "utilities/Logger.h"

#include <regex>

#include <p8-platform/util/StringUtils.h>

using namespace iptvsimple;
using namespace iptvsimple::data;
using namespace iptvsimple::utilities;

CatchupController::CatchupController(Epg& epg, std::mutex* mutex)
  : m_epg(epg), m_mutex(mutex) {}

void CatchupController::ProcessChannelForPlayback(Channel& channel, std::map<std::string, std::string>& catchupProperties) // TODO: should be a const channel, create a StreamManager instead
{
  TestAndStoreStreamType(channel); // TODO: StreamManager

  // Anything from here is live!
  m_playbackIsVideo = false; // TODO: possible time jitter on UI as this will effect get stream times

  if (!m_fromEpgTag || m_controlsLiveStream)
  {
    EpgEntry* liveEpgEntry = GetLiveEPGEntry(channel);
    if (liveEpgEntry && !Settings::GetInstance().CatchupOnlyOnFinishedProgrammes())
    {
      // Live with EPG entry
      UpdateProgrammeFrom(*liveEpgEntry, channel.GetTvgShift());
      m_catchupStartTime = liveEpgEntry->GetStartTime();
      m_catchupEndTime = liveEpgEntry->GetEndTime();
    }
    else if (m_controlsLiveStream || !channel.IsCatchupSupported() ||
             (channel.IsCatchupSupported() && Settings::GetInstance().CatchupOnlyOnFinishedProgrammes()))
    {
      // Live without EPG entry
      ClearProgramme();
      m_catchupStartTime = 0;
      m_catchupEndTime = 0;
    }
    m_fromEpgTag = false;
  }

  if (m_controlsLiveStream)
  {
    if (m_resetCatchupState)
    {
      m_resetCatchupState = false;
      if (channel.IsCatchupSupported())
      {
        m_timeshiftBufferOffset = Settings::GetInstance().GetCatchupDaysInSeconds(); //offset from now to start of catchup window
        m_timeshiftBufferStartTime = std::time(nullptr) - Settings::GetInstance().GetCatchupDaysInSeconds(); // now - the window size
      }
      else
      {
        m_timeshiftBufferOffset = 0;
        m_timeshiftBufferStartTime = 0;
      }
    }
    else
    {
      EpgEntry* currentEpgEntry = GetEPGEntry(channel, m_timeshiftBufferStartTime + m_timeshiftBufferOffset);
      if (currentEpgEntry)
        UpdateProgrammeFrom(*currentEpgEntry, channel.GetTvgShift());
    }

    m_catchupStartTime = m_timeshiftBufferStartTime;

    // TODO: Need a method of updating an inputstream if already running such as web call to stream etc.
    // this will avoid inputstream restarts which are expensive, may be better placed in client.cpp
    // this also mean knowing when a stream has stopped
    SetCatchupInputStreamProperties(true, channel, catchupProperties);
  }
}

void CatchupController::ProcessEPGTagForTimeshiftedPlayback(const EPG_TAG& epgTag, Channel& channel, std::map<std::string, std::string>& catchupProperties) // TODO: should be a const channel, create a StreamManager instead
{
  TestAndStoreStreamType(channel); // TODO: StreamManager

  if (m_controlsLiveStream)
  {
    UpdateProgrammeFrom(epgTag, channel.GetTvgShift());
    m_catchupStartTime = epgTag.startTime;
    m_catchupEndTime = epgTag.endTime;

    time_t timeNow = time(0);
    time_t programmeOffset = timeNow - m_catchupStartTime;
    time_t timeshiftBufferDuration = std::max(programmeOffset, Settings::GetInstance().GetCatchupDaysInSeconds());
    m_timeshiftBufferStartTime = timeNow - timeshiftBufferDuration;
    m_catchupStartTime = m_timeshiftBufferStartTime;
    m_catchupEndTime = timeNow;
    m_timeshiftBufferOffset = timeshiftBufferDuration - programmeOffset;

    m_resetCatchupState = false;

    // TODO: Need a method of updating an inputstream if already running such as web call to stream etc.
    // this will avoid inputstream restarts which are expensive, may be better placed in client.cpp
    // this also mean knowing when a stream has stopped
    SetCatchupInputStreamProperties(true, channel, catchupProperties);
  }
  else
  {
    UpdateProgrammeFrom(epgTag, channel.GetTvgShift());
    m_catchupStartTime = epgTag.startTime;
    m_catchupEndTime = epgTag.endTime;

    m_timeshiftBufferStartTime = 0;
    m_timeshiftBufferOffset = 0;

    m_fromEpgTag = true;
  }
}

void CatchupController::ProcessEPGTagForVideoPlayback(const EPG_TAG& epgTag, Channel& channel, std::map<std::string, std::string>& catchupProperties) // TODO: should be a const channel, create a StreamManager instead
{
  TestAndStoreStreamType(channel); // TODO: StreamManager

  if (m_controlsLiveStream)
  {
    if (m_resetCatchupState)
    {
      UpdateProgrammeFrom(epgTag, channel.GetTvgShift());
      m_catchupStartTime = epgTag.startTime;
      m_catchupEndTime = epgTag.endTime;

      const time_t beginBuffer = Settings::GetInstance().GetCatchupWatchEpgBeginBufferSecs();
      const time_t endBuffer = Settings::GetInstance().GetCatchupWatchEpgEndBufferSecs();
      m_timeshiftBufferStartTime = m_catchupStartTime - beginBuffer;
      m_catchupStartTime = m_timeshiftBufferStartTime;
      m_catchupEndTime += endBuffer;
      m_timeshiftBufferOffset = beginBuffer;

      m_resetCatchupState = false;
    }

    // TODO: Need a method of updating an inputstream if already running such as web call to stream etc.
    // this will avoid inputstream restarts which are expensive, may be better placed in client.cpp
    // this also mean knowing when a stream has stopped
    SetCatchupInputStreamProperties(false, channel, catchupProperties);
  }
  else
  {
    UpdateProgrammeFrom(epgTag, channel.GetTvgShift());
    m_catchupStartTime = epgTag.startTime;
    m_catchupEndTime = epgTag.endTime;

    m_timeshiftBufferStartTime = 0;
    m_timeshiftBufferOffset = 0;
    m_catchupStartTime = m_catchupStartTime - Settings::GetInstance().GetCatchupWatchEpgBeginBufferSecs();
    m_catchupEndTime += Settings::GetInstance().GetCatchupWatchEpgEndBufferSecs();
  }

  if (m_catchupStartTime > 0)
    m_playbackIsVideo = true;
}

void CatchupController::SetCatchupInputStreamProperties(bool playbackAsLive, const Channel& channel, std::map<std::string, std::string>& catchupProperties)
{
  catchupProperties.insert({"epgplaybackaslive", playbackAsLive ? "true" : "false"});

  catchupProperties.insert({"inputstream.ffmpegdirect.is_realtime_stream", "true"});
  catchupProperties.insert({"inputstream.ffmpegdirect.stream_mode", "catchup"});

  catchupProperties.insert({"inputstream.ffmpegdirect.default_url", channel.GetStreamURL()});
  catchupProperties.insert({"inputstream.ffmpegdirect.playback_as_live", playbackAsLive ? "true" : "false"});
  catchupProperties.insert({"inputstream.ffmpegdirect.catchup_url_format_string", GetCatchupUrlFormatString(channel)});
  catchupProperties.insert({"inputstream.ffmpegdirect.catchup_buffer_start_time", std::to_string(m_catchupStartTime)});
  catchupProperties.insert({"inputstream.ffmpegdirect.catchup_buffer_end_time", std::to_string(m_catchupEndTime)});
  catchupProperties.insert({"inputstream.ffmpegdirect.catchup_buffer_offset", std::to_string(m_timeshiftBufferOffset)});
  catchupProperties.insert({"inputstream.ffmpegdirect.timezone_shift", std::to_string(channel.GetTvgShift())});

  // TODO: Should also send programme start and duration potentially
  // When doing this don't forget to add Settings::GetInstance().GetCatchupWatchEpgBeginBufferSecs() + Settings::GetInstance().GetCatchupWatchEpgEndBufferSecs();
  // if in video playbacl mode from epg, i.e. if if (!Settings::GetInstance().CatchupPlayEpgAsLive() && m_playbackIsVideo)s

  Logger::Log(LEVEL_DEBUG, "default_url - %s", channel.GetStreamURL().c_str());
  Logger::Log(LEVEL_DEBUG, "playback_as_live - %s", playbackAsLive ? "true" : "false");
  Logger::Log(LEVEL_DEBUG, "catchup_url_format_string - %s", GetCatchupUrlFormatString(channel).c_str());
  Logger::Log(LEVEL_DEBUG, "catchup_buffer_start_time - %s", std::to_string(m_catchupStartTime).c_str());
  Logger::Log(LEVEL_DEBUG, "catchup_buffer_end_time - %s", std::to_string(m_catchupEndTime).c_str());
  Logger::Log(LEVEL_DEBUG, "catchup_buffer_offset - %s", std::to_string(m_timeshiftBufferOffset).c_str());
  Logger::Log(LEVEL_DEBUG, "timezone_shift - %s", std::to_string(channel.GetTvgShift()).c_str());
}

void CatchupController::TestAndStoreStreamType(Channel& channel)
{
  // We need to find out what type of stream this is, so let's construct a catchup URL to test with
  const std::string streamTestUrl = GetStreamTestUrl(channel);
  StreamType streamType = StreamUtils::GetStreamType(streamTestUrl, channel);
  if (streamType == StreamType::OTHER_TYPE)
    streamType = StreamUtils::InspectStreamType(streamTestUrl);

  // TODO: we really want to store this in a file and load it on any restart
  // using channel doesn't make sense as it's otherwise immutable.

  // If we find a mimetype store it so we don't have to look it up again
  if (!channel.GetProperty("mimetype").empty() && (streamType == StreamType::HLS || streamType == StreamType::DASH))
  {
    std::lock_guard<std::mutex> lock(*m_mutex);
    channel.AddProperty("mimetype", StreamUtils::GetMimeType(streamType));
  }

  if (StreamUtils::GetEffectiveInputStreamClass(streamType, channel) == "inputstream.ffmpegdirect")
    m_controlsLiveStream = true;
  else
    m_controlsLiveStream = false;
}

void CatchupController::UpdateProgrammeFrom(const EPG_TAG& epgTag, int tvgShift)
{
  m_programmeStartTime = epgTag.startTime;
  m_programmeEndTime = epgTag.endTime;
  m_programmeTitle = epgTag.strTitle;
  m_programmeUniqueChannelId = epgTag.iUniqueChannelId;
  m_programmeChannelTvgShift = tvgShift;
}

void CatchupController::UpdateProgrammeFrom(const data::EpgEntry& epgEntry, int tvgShift)
{
  m_programmeStartTime = epgEntry.GetStartTime();
  m_programmeEndTime = epgEntry.GetEndTime();
  m_programmeTitle = epgEntry.GetTitle();
  m_programmeUniqueChannelId = epgEntry.GetChannelId();
  m_programmeChannelTvgShift = tvgShift;
}

void CatchupController::ClearProgramme()
{
  m_programmeStartTime = 0;
  m_programmeEndTime = 0;
  m_programmeTitle.clear();
  m_programmeUniqueChannelId = 0;
  m_programmeChannelTvgShift = 0;
}

namespace
{
void FormatOffset(time_t tTime, std::string &urlFormatString)
{
  static const std::regex OFFSET_REGEX(".*(\\{offset:(\\d+)\\}).*");
  std::cmatch mr;
  if (std::regex_match(urlFormatString.c_str(), mr, OFFSET_REGEX) && mr.length() >= 3)
  {
    std::string offsetExp = mr[1].first;
    std::string second = mr[1].second;
    if (second.length() > 0)
      offsetExp = offsetExp.erase(offsetExp.find(second));
    std::string dividerStr = mr[2].first;
    second = mr[2].second;
    if (second.length() > 0)
      dividerStr = dividerStr.erase(dividerStr.find(second));

    const time_t divider = stoi(dividerStr);
    if (divider != 0)
    {
      time_t offset = tTime / divider;
      if (offset < 0)
        offset = 0;
      urlFormatString.replace(urlFormatString.find(offsetExp), offsetExp.length(), std::to_string(offset));
    }
  }
}

void FormatTime(const char ch, const struct tm *pTime, std::string &urlFormatString)
{
  char str[] = { '{', ch, '}', 0 };
  auto pos = urlFormatString.find(str);
  if (pos != std::string::npos)
  {
    char buff[256], timeFmt[3];
    std::snprintf(timeFmt, sizeof(timeFmt), "%%%c", ch);
    std::strftime(buff, sizeof(buff), timeFmt, pTime);
    if (std::strlen(buff) > 0)
      urlFormatString.replace(pos, 3, buff);
  }
}

void FormatUtc(const char *str, time_t tTime, std::string &urlFormatString)
{
  auto pos = urlFormatString.find(str);
  if (pos != std::string::npos)
  {
    char buff[256];
    std::snprintf(buff, sizeof(buff), "%lu", tTime);
    urlFormatString.replace(pos, std::strlen(str), buff);
  }
}

std::string FormatDateTime(time_t dateTimeEpg, time_t duration, const std::string &urlFormatString)
{
  std::string fomrattedUrl = urlFormatString;

  const time_t dateTimeNow = std::time(0);
  tm* dateTime = std::localtime(&dateTimeEpg);

  FormatTime('Y', dateTime, fomrattedUrl);
  FormatTime('m', dateTime, fomrattedUrl);
  FormatTime('d', dateTime, fomrattedUrl);
  FormatTime('H', dateTime, fomrattedUrl);
  FormatTime('M', dateTime, fomrattedUrl);
  FormatTime('S', dateTime, fomrattedUrl);
  FormatUtc("{utc}", dateTimeEpg, fomrattedUrl);
  FormatUtc("${start}", dateTimeEpg, fomrattedUrl);
  FormatUtc("{utcend}", dateTimeEpg + duration, fomrattedUrl);
  FormatUtc("${end}", dateTimeEpg + duration, fomrattedUrl);
  FormatUtc("{lutc}", dateTimeNow, fomrattedUrl);
  FormatUtc("{duration}", duration, fomrattedUrl);
  FormatOffset(dateTimeNow - dateTimeEpg, fomrattedUrl);

  Logger::Log(LEVEL_DEBUG, "%s - \"%s\"", __FUNCTION__, fomrattedUrl.c_str());

  return fomrattedUrl;
}

std::string AppendQueryStringAndPreserveOptions(const std::string &url, const std::string &postfixQueryString)
{
  std::string urlFormatString;
  if (!postfixQueryString.empty())
  {
    // preserve any kodi protocol options after "|"
    size_t found = url.find_first_of('|');
    if (found != std::string::npos)
      urlFormatString = url.substr(0, found) + postfixQueryString + url.substr(found, url.length());
    else
      urlFormatString = url + postfixQueryString;
  }
  else
  {
    urlFormatString = url;
  }

  return urlFormatString;
}

std::string BuildUrlFormatString(const Channel& channel)
{
  std::string urlFormatString;

  if (!channel.GetCatchupSource().empty())
  {
    if (channel.GetCatchupMode() == CatchupMode::DEFAULT)
      urlFormatString = channel.GetCatchupSource();
    else // source is query to be appended
      urlFormatString = AppendQueryStringAndPreserveOptions(channel.GetStreamURL(), channel.GetCatchupSource());
  }
  else
  {
    urlFormatString = AppendQueryStringAndPreserveOptions(channel.GetStreamURL(), Settings::GetInstance().GetCatchupQueryFormat());
  }

  return urlFormatString;
}

std::string BuildEpgTagUrl(time_t startTime, time_t duration, const Channel& channel, long long timeOffset)
{
  std::string startTimeUrl;
  time_t timeNow = time(0);
  time_t offset = startTime + timeOffset;

  if (startTime > 0 && offset < (timeNow - 5))
    startTimeUrl = FormatDateTime(offset - channel.GetTvgShift(), duration, BuildUrlFormatString(channel));
  else
    startTimeUrl = channel.GetStreamURL();

  Logger::Log(LEVEL_DEBUG, "%s - %s", __FUNCTION__, startTimeUrl.c_str());

  return startTimeUrl;
}

} // unnamed namespace

std::string CatchupController::GetCatchupUrlFormatString(const Channel& channel) const
{
  if (m_catchupStartTime > 0)
    return BuildUrlFormatString(channel);

  return "";
}

std::string CatchupController::GetCatchupUrl(const Channel& channel) const
{
  if (m_catchupStartTime > 0)
  {
    time_t duration = static_cast<time_t>(m_programmeEndTime - m_programmeStartTime);

    if (!Settings::GetInstance().CatchupPlayEpgAsLive() && m_playbackIsVideo)
      duration += Settings::GetInstance().GetCatchupWatchEpgBeginBufferSecs() + Settings::GetInstance().GetCatchupWatchEpgEndBufferSecs();

    return BuildEpgTagUrl(m_catchupStartTime, duration, channel, m_timeshiftBufferOffset);
  }

  return "";
}

std::string CatchupController::GetStreamTestUrl(const Channel& channel) const
{
  // Test URL from 2 hours ago for 1 hour duration.
  return BuildEpgTagUrl(std::time(nullptr) - (2 * 60 * 60),  60 * 60, channel, 0);
}

EpgEntry* CatchupController::GetLiveEPGEntry(const Channel& myChannel)
{
  std::lock_guard<std::mutex> lock(*m_mutex);

  return m_epg.GetLiveEPGEntry(myChannel);
}

EpgEntry* CatchupController::GetEPGEntry(const Channel& myChannel, time_t lookupTime)
{
  std::lock_guard<std::mutex> lock(*m_mutex);

  return m_epg.GetEPGEntry(myChannel, lookupTime);
}
