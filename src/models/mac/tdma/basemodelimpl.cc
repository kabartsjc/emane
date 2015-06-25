/*
 * Copyright (c) 2015 - Adjacent Link LLC, Bridgewater, New Jersey
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 * * Neither the name of Adjacent Link LLC nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "basemodelimpl.h"
#include "emane/models/tdma/queuemanager.h"

#include "emane/configureexception.h"
#include "emane/controls/frequencyofinterestcontrolmessage.h"
#include "emane/controls/flowcontrolcontrolmessage.h"
#include "emane/controls/serializedcontrolmessage.h"
#include "emane/mactypes.h"

#include "emane/controls/frequencycontrolmessage.h"
#include "emane/controls/frequencycontrolmessageformatter.h"
#include "emane/controls/receivepropertiescontrolmessage.h"
#include "emane/controls/receivepropertiescontrolmessageformatter.h"
#include "emane/controls/timestampcontrolmessage.h"


#include "txslotinfosformatter.h"
#include "basemodelmessage.h"
#include "priority.h"

namespace
{
  const std::string QUEUEMANAGER_PREFIX{"queue."};
  const std::string SCHEDULER_PREFIX{"scheduler."};
}

EMANE::Models::TDMA::BaseModel::Implementation::
Implementation(NEMId id,
               PlatformServiceProvider *pPlatformServiceProvider,
               RadioServiceProvider * pRadioServiceProvider,
               Scheduler * pScheduler,
               QueueManager * pQueueManager,
               MACLayerImplementor * pRadioModel):
  MACLayerImplementor{id,pPlatformServiceProvider,pRadioServiceProvider},
  pScheduler_{pScheduler},
  pQueueManager_{pQueueManager},
  pRadioModel_{pRadioModel},
  bFlowControlEnable_{},
  u16FlowControlTokens_{},
  bRadioMetricEnable_{},
  radioMetricReportIntervalMicroseconds_{},
  sPCRCurveURI_{},
  transmitTimedEventId_{},
  nextMultiFrameTime_{},
  txSlotInfos_{},
  slotDuration_{},
  slotOverhead_{},
  u64SequenceNumber_{},
  frequencies_{},
  u64BandwidthHz_{},
  packetStatusPublisher_{},
  receiveManager_{id,
      pRadioModel,
      &pPlatformServiceProvider->logService(),
      pRadioServiceProvider,
      pScheduler,
      &packetStatusPublisher_},
  flowControlManager_{*pRadioModel}{}


EMANE::Models::TDMA::BaseModel::Implementation::~Implementation()
{}


void
EMANE::Models::TDMA::BaseModel::Implementation::initialize(Registrar & registrar)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);

  auto & configRegistrar = registrar.configurationRegistrar();

  configRegistrar.registerNumeric<bool>("enablepromiscuousmode",
                                        ConfigurationProperties::DEFAULT |
                                        ConfigurationProperties::MODIFIABLE,
                                        {false},
                                        "Defines whether promiscuous mode is enabled or not."
                                        " If promiscuous mode is enabled, all received packets"
                                        " (intended for the given node or not) that pass the"
                                        " probability of reception check are sent upstream to"
                                        " the transport.");


  configRegistrar.registerNumeric<bool>("flowcontrolenable",
                                        ConfigurationProperties::DEFAULT,
                                        {false},
                                        "Defines whether flow control is enabled. Flow control only works"
                                        " with the virtual transport and the setting must match the setting"
                                        " within the virtual transport configuration.");

  configRegistrar.registerNumeric<std::uint16_t>("flowcontroltokens",
                                                 ConfigurationProperties::DEFAULT,
                                                 {10},
                                                 "Defines the maximum number of flow control tokens"
                                                 " (packet transmission units) that can be processed from the"
                                                 " virtual transport without being refreshed. The number of"
                                                 " available tokens at any given time is coordinated with the"
                                                 " virtual transport and when the token count reaches zero, no"
                                                 " further packets are transmitted causing application socket"
                                                 " queues to backup.");

  configRegistrar.registerNumeric<bool>("radiometricenable",
                                        ConfigurationProperties::DEFAULT,
                                        {false},
                                        "Defines if radio metrics will be reported up via the Radio to Router Interface"
                                        " (R2RI).");


  configRegistrar.registerNumeric<float>("radiometricreportinterval",
                                         ConfigurationProperties::DEFAULT,
                                         {1.0f},
                                         "Defines the metric report interval in seconds in support of the R2RI feature.",
                                         0.1f,
                                         60.0f);


  configRegistrar.registerNonNumeric<std::string>("pcrcurveuri",
                                                  ConfigurationProperties::REQUIRED,
                                                  {},
                                                  "Defines the absolute URI of the Packet Completion Rate (PCR) curve"
                                                  " file. The PCR curve file contains probability of reception curves"
                                                  " as a function of Signal to Interference plus Noise Ratio (SINR).");


  configRegistrar.registerNumeric<std::uint16_t>("fragmentcheckthreshold",
                                                 ConfigurationProperties::DEFAULT,
                                                 {2},
                                                 "Defines the rate in seconds a check is performed see if any packet"
                                                 " fragment reassembly efforts should be abandoned.");

  configRegistrar.registerNumeric<std::uint16_t>("fragmenttimeoutthreshold",
                                                 ConfigurationProperties::DEFAULT,
                                                 {5},
                                                 "Defines the threshold in seconds to wait for another packet fragment"
                                                 " for an existing reassembly effort before abandoning the effort.");



  auto & statisticRegistrar = registrar.statisticRegistrar();

  packetStatusPublisher_.registerStatistics(statisticRegistrar);

  slotStatusTablePublisher_.registerStatistics(statisticRegistrar);

  pQueueManager_->setPacketStatusPublisher(&packetStatusPublisher_);

  pQueueManager_->initialize(registrar);

  pScheduler_->initialize(registrar);
}



void
EMANE::Models::TDMA::BaseModel::Implementation::configure(const ConfigurationUpdate & update)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);

  ConfigurationUpdate schedulerConfiguration{};
  ConfigurationUpdate queueManagerConfiguration{};

  for(const auto & item : update)
    {
      if(item.first == "enablepromiscuousmode")
        {
          bool bPromiscuousMode{item.second[0].asBool()};

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu TDMA::BaseModel::%s: %s = %s",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  bPromiscuousMode ? "on" : "off");

          receiveManager_.setPromiscuousMode(bPromiscuousMode);
        }
      else if(item.first == "flowcontrolenable")
        {
          bFlowControlEnable_ = item.second[0].asBool();

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu TDMA::BaseModel::%s: %s = %s",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  bFlowControlEnable_ ? "on" : "off");
        }
      else if(item.first == "flowcontroltokens")
        {
          u16FlowControlTokens_ = item.second[0].asUINT16();

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu TDMA::BaseModel::%s: %s = %hu",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  u16FlowControlTokens_);
        }
      else if(item.first == "radiometricenable")
        {
          bRadioMetricEnable_ = item.second[0].asBool();

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu TDMA::BaseModel::%s: %s = %s",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  bRadioMetricEnable_ ? "on" : "off");
        }
      else if(item.first == "radiometricreportinterval")
        {
          radioMetricReportIntervalMicroseconds_ =
            std::chrono::duration_cast<Microseconds>(DoubleSeconds{item.second[0].asFloat()});

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu TDMA::BaseModel::%s: %s = %lf usec",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  std::chrono::duration_cast<DoubleSeconds>(radioMetricReportIntervalMicroseconds_).count());
        }
      else if(item.first == "pcrcurveuri")
        {
          sPCRCurveURI_ = item.second[0].asString();

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu TDMA::BaseModel::%s: %s = %s",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  sPCRCurveURI_.c_str());

          receiveManager_.loadCurves(sPCRCurveURI_);
        }
      else if(item.first == "fragmentcheckthreshold")
        {
          std::chrono::seconds fragmentCheckThreshold{item.second[0].asUINT16()};

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu TDMA::BaseModel::%s: %s = %lu",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  fragmentCheckThreshold.count());

          receiveManager_.setFragmentCheckThreshold(fragmentCheckThreshold);
        }
      else if(item.first == "fragmenttimeoutthreshold")
        {
          std::chrono::seconds fragmentTimeoutThreshold{item.second[0].asUINT16()};

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu TDMA::BaseModel::%s: %s = %lu",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  fragmentTimeoutThreshold.count());

          receiveManager_.setFragmentTimeoutThreshold(fragmentTimeoutThreshold);
        }
      else
        {
          if(!item.first.compare(0,SCHEDULER_PREFIX.size(),SCHEDULER_PREFIX))
            {
              schedulerConfiguration.push_back(item);
            }
          else if(!item.first.compare(0,QUEUEMANAGER_PREFIX.size(),QUEUEMANAGER_PREFIX))
            {
              queueManagerConfiguration.push_back(item);
            }
          else
            {
              throw makeException<ConfigureException>("TDMA::BaseModel: "
                                                      "Ambiguous configuration item %s.",
                                                      item.first.c_str());
            }
        }
    }

  pQueueManager_->configure(queueManagerConfiguration);

  pScheduler_->configure(schedulerConfiguration);
}

void
EMANE::Models::TDMA::BaseModel::Implementation::start()
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);

  pQueueManager_->start();

  pScheduler_->start();
}


void
EMANE::Models::TDMA::BaseModel::Implementation::postStart()
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);

  pQueueManager_->postStart();

  pScheduler_->postStart();

  // check flow control enabled
  if(bFlowControlEnable_)
    {
      // start flow control
      flowControlManager_.start(u16FlowControlTokens_);

      LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                              DEBUG_LEVEL,
                              "MACI %03hu TDMA::BaseModel::%s sent a flow control token update,"
                              " a handshake response is required to process packets",
                              id_,
                              __func__);
    }

}


void
EMANE::Models::TDMA::BaseModel::Implementation::stop()
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);

  // check flow control enabled
  if(bFlowControlEnable_)
    {
      // stop the flow control manager
      flowControlManager_.stop();
    }

  pQueueManager_->stop();

  pScheduler_->stop();
}



void
EMANE::Models::TDMA::BaseModel::Implementation::destroy()
  throw()
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);

  pQueueManager_->destroy();

  pScheduler_->destroy();
}

void EMANE::Models::TDMA::BaseModel::Implementation::processUpstreamControl(const ControlMessages &)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);

}


void EMANE::Models::TDMA::BaseModel::Implementation::processUpstreamPacket(const CommonMACHeader & hdr,
                                                                           UpstreamPacket & pkt,
                                                                           const ControlMessages & msgs)
{
  auto now = Clock::now();

  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);


  const PacketInfo & pktInfo{pkt.getPacketInfo()};

  if(hdr.getRegistrationId() != REGISTERED_EMANE_MAC_TDMA)
    {
      LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                              ERROR_LEVEL,
                              "MACI %03hu TDMA::BaseModel::%s: MAC Registration Id %hu does not match our Id %hu, drop.",
                              id_,
                              __func__,
                              hdr.getRegistrationId(),
                              REGISTERED_EMANE_MAC_TDMA);


      packetStatusPublisher_.inbound(pktInfo.getSource(),
                                     pktInfo.getSource(),
                                     pktInfo.getPriority(),
                                     pkt.length(),
                                     PacketStatusPublisher::InboundAction::DROP_REGISTRATION_ID);

      // drop
      return;
    }

  size_t len{pkt.stripLengthPrefixFraming()};

  if(len && pkt.length() >= len)
    {
      BaseModelMessage baseModelMessage{pkt.get(), len};

      auto entry = pScheduler_->getRxSlotInfo(now);

      if(entry.first.u64AbsoluteSlotIndex_ == baseModelMessage.getAbsoluteSlotIndex())
        {
          Microseconds timeRemainingInSlot{slotDuration_ -
              std::chrono::duration_cast<Microseconds>(now -
                                                       entry.first.timePoint_)};

          double dSlotRemainingRatio =
            timeRemainingInSlot.count() / static_cast<double>(slotDuration_.count());

          if(entry.second)
            {
              const Controls::ReceivePropertiesControlMessage * pReceivePropertiesControlMessage{};

              const Controls::FrequencyControlMessage * pFrequencyControlMessage{};

              for(auto & pControlMessage : msgs)
                {
                  switch(pControlMessage->getId())
                    {
                    case EMANE::Controls::ReceivePropertiesControlMessage::IDENTIFIER:
                      {
                        pReceivePropertiesControlMessage =
                          static_cast<const Controls::ReceivePropertiesControlMessage *>(pControlMessage);

                        LOGGER_VERBOSE_LOGGING_FN_VARGS(pPlatformService_->logService(),
                                                        DEBUG_LEVEL,
                                                        Controls::ReceivePropertiesControlMessageFormatter(pReceivePropertiesControlMessage),
                                                        "MACI %03hu TDMA::BaseModel::%s Receiver Properties Control Message",
                                                        id_,
                                                        __func__);
                      }
                      break;

                    case Controls::FrequencyControlMessage::IDENTIFIER:
                      {
                        pFrequencyControlMessage =
                          static_cast<const Controls::FrequencyControlMessage *>(pControlMessage);

                        LOGGER_VERBOSE_LOGGING_FN_VARGS(pPlatformService_->logService(),
                                                        DEBUG_LEVEL,
                                                        Controls::FrequencyControlMessageFormatter(pFrequencyControlMessage),
                                                        "MACI %03hu TDMA::BaseModel::%s Frequency Control Message",
                                                        id_,
                                                        __func__);

                      }

                      break;
                    }
                }

              if(!pReceivePropertiesControlMessage || !pFrequencyControlMessage ||
                 pFrequencyControlMessage->getFrequencySegments().empty())
                {
                  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                          ERROR_LEVEL,
                                          "MACI %03hu TDMA::BaseModel::%s: phy control "
                                          "message not provided from src %hu, drop",
                                          id_,
                                          __func__,
                                          pktInfo.getSource());

                  packetStatusPublisher_.inbound(pktInfo.getSource(),
                                                 baseModelMessage.getMessages(),
                                                 PacketStatusPublisher::InboundAction::DROP_BAD_CONTROL);

                  // drop
                  return;
                }
              const auto & frequencySegments = pFrequencyControlMessage->getFrequencySegments();

              const FrequencySegment & frequencySegment{*frequencySegments.begin()};

              if(entry.first.u64FrequencyHz_ == frequencySegment.getFrequencyHz())
                {
                  // we are in an RX Slot
                  slotStatusTablePublisher_.update(entry.first.u32RelativeIndex_,
                                                   entry.first.u32RelativeFrameIndex_,
                                                   entry.first.u32RelativeSlotIndex_,
                                                   SlotStatusTablePublisher::Status::RX_GOOD,
                                                   dSlotRemainingRatio);
                }


              TimePoint startOfReception{pReceivePropertiesControlMessage->getTxTime() +
                  pReceivePropertiesControlMessage->getPropagationDelay() +
                  frequencySegment.getOffset()};


              Microseconds span{pReceivePropertiesControlMessage->getSpan()};

              if(receiveManager_.enqueue(std::move(baseModelMessage),
                                         pktInfo,
                                         pkt.length(),
                                         startOfReception,
                                         frequencySegments,
                                         span,
                                         now))
                {
                  pPlatformService_->timerService().
                    scheduleTimedEvent(entry.first.timePoint_+ slotDuration_,
                                       new std::function<bool()>{std::bind([](ReceiveManager  & receiveManager,
                                                                              std::uint64_t u64AbsoluteSlotIndex)
                                                                           {
                                                                             receiveManager.process(u64AbsoluteSlotIndex);
                                                                             return true;
                                                                           },std::ref(receiveManager_),
                                                                           entry.first.u64AbsoluteSlotIndex_+1)});
                }
            }
          else
            {
              // not an rx slot but it is the correct abs slot
              auto pktSlotInfo = pScheduler_->getSlotInfo(baseModelMessage.getAbsoluteSlotIndex());

              slotStatusTablePublisher_.update(entry.first.u32RelativeIndex_,
                                               entry.first.u32RelativeFrameIndex_,
                                               entry.first.u32RelativeSlotIndex_,
                                               pktSlotInfo.type_ == SlotInfo::Type::IDLE ?
                                               SlotStatusTablePublisher::Status::RX_IDLE :
                                               SlotStatusTablePublisher::Status::RX_TX,
                                               dSlotRemainingRatio);

              packetStatusPublisher_.inbound(pktInfo.getSource(),
                                             baseModelMessage.getMessages(),
                                             PacketStatusPublisher::InboundAction::DROP_SLOT_NOT_RX);


              LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                      DEBUG_LEVEL,
                                      "MACI %03hu TDMA::BaseModel::%s drop reason rx slot correct but %s rframe: %u rslot: %u",
                                      id_,
                                      __func__,
                                      pktSlotInfo.type_ == SlotInfo::Type::IDLE ?
                                      "in idle" : "in tx",
                                      entry.first.u32RelativeFrameIndex_,
                                      entry.first.u32RelativeSlotIndex_);
            }
        }
      else
        {
          auto pktSlotInfo = pScheduler_->getSlotInfo(baseModelMessage.getAbsoluteSlotIndex());

          Microseconds timeRemainingInSlot{slotDuration_ -
              std::chrono::duration_cast<Microseconds>(now -
                                                       pktSlotInfo.timePoint_)};
          double dSlotRemainingRatio =
            timeRemainingInSlot.count() / static_cast<double>(slotDuration_.count());


          // were we supposed to be in rx on the pkt abs slot
          if(pktSlotInfo.type_ == SlotInfo::Type::RX)
            {
              slotStatusTablePublisher_.update(entry.first.u32RelativeIndex_,
                                               entry.first.u32RelativeFrameIndex_,
                                               entry.first.u32RelativeSlotIndex_,
                                               SlotStatusTablePublisher::Status::RX_MISSED,
                                               dSlotRemainingRatio);

              packetStatusPublisher_.inbound(pktInfo.getSource(),
                                             baseModelMessage.getMessages(),
                                             PacketStatusPublisher::InboundAction::DROP_SLOT_MISSED_RX);

              LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                      DEBUG_LEVEL,
                                      "MACI %03hu TDMA::BaseModel::%s drop reason slot mismatch pkt: %zu now: %zu ",
                                      id_,
                                      __func__,
                                      baseModelMessage.getAbsoluteSlotIndex(),
                                      entry.first.u64AbsoluteSlotIndex_);
            }
          else
            {
              slotStatusTablePublisher_.update(entry.first.u32RelativeIndex_,
                                               entry.first.u32RelativeFrameIndex_,
                                               entry.first.u32RelativeSlotIndex_,
                                               pktSlotInfo.type_ == SlotInfo::Type::IDLE ?
                                               SlotStatusTablePublisher::Status::RX_IDLE :
                                               SlotStatusTablePublisher::Status::RX_TX,
                                               dSlotRemainingRatio);

              packetStatusPublisher_.inbound(pktInfo.getSource(),
                                             baseModelMessage.getMessages(),
                                             PacketStatusPublisher::InboundAction::DROP_SLOT_NOT_RX);


              LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                      DEBUG_LEVEL,
                                      "MACI %03hu TDMA::BaseModel::%s drop reason slot mismatch but %s pkt: %zu now: %zu ",
                                      id_,
                                      __func__,
                                      pktSlotInfo.type_ == SlotInfo::Type::IDLE ?
                                      "in idle" : "in tx",
                                      baseModelMessage.getAbsoluteSlotIndex(),
                                      entry.first.u64AbsoluteSlotIndex_);
            }
        }
    }
  else
    {
      LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                              ERROR_LEVEL,
                              "MACI %03hu TDMA::BaseModel::%s Packet payload length %zu does not match length prefix %zu",
                              id_,
                              __func__,
                              pkt.length(),
                              len);
    }
}

void EMANE::Models::TDMA::BaseModel::Implementation::processDownstreamControl(const ControlMessages & msgs)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);

  for(const auto & pMessage : msgs)
    {
      LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                              DEBUG_LEVEL,
                              "MACI %03hu TDMA::BaseModel::%s downstream control message id %hu",
                              id_,
                              __func__,
                              pMessage->getId());

      switch(pMessage->getId())
        {
        case Controls::FlowControlControlMessage::IDENTIFIER:
          {
            const auto pFlowControlControlMessage =
              static_cast<const Controls::FlowControlControlMessage *>(pMessage);

            if(bFlowControlEnable_)
              {
                LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                        DEBUG_LEVEL,
                                        "MACI %03hu TDMA::BaseModel::%s received a flow control token request/response",
                                        id_,
                                        __func__);

                flowControlManager_.processFlowControlMessage(pFlowControlControlMessage);
              }
            else
              {
                LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                        ERROR_LEVEL,
                                        "MACI %03hu TDMA::BaseModel::%s received a flow control token request but"
                                        " flow control is not enabled",
                                        id_,
                                        __func__);
              }
          }
          break;

        case Controls::SerializedControlMessage::IDENTIFIER:
          {
            const auto pSerializedControlMessage =
              static_cast<const Controls::SerializedControlMessage *>(pMessage);

            switch(pSerializedControlMessage->getSerializedId())
              {
              case Controls::FlowControlControlMessage::IDENTIFIER:
                {
                  std::unique_ptr<Controls::FlowControlControlMessage>
                    pFlowControlControlMessage{
                    Controls::FlowControlControlMessage::create(pSerializedControlMessage->getSerialization())};

                  if(bFlowControlEnable_)
                    {
                      flowControlManager_.processFlowControlMessage(pFlowControlControlMessage.get());
                    }
                  else
                    {
                      LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                              ERROR_LEVEL,
                                              "MACI %03hu TDMA::BaseModel::%s received a flow control token request but"
                                              " flow control is not enabled",
                                              id_,
                                              __func__);
                    }
                }
                break;
              }
          }
        }
    }
}


void EMANE::Models::TDMA::BaseModel::Implementation::processDownstreamPacket(DownstreamPacket & pkt,
                                                                             const ControlMessages &)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);


  // check flow control
  if(bFlowControlEnable_)
    {
      auto status = flowControlManager_.removeToken();

      if(status.second == false)
        {
          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  ERROR_LEVEL,
                                  "MACI %03hu TDMA::BaseModel::%s: failed to remove token, drop packet (tokens:%hu)",
                                  id_,
                                  __func__,
                                  status.first);

          const auto & pktInfo = pkt.getPacketInfo();

          packetStatusPublisher_.outbound(pktInfo.getSource(),
                                          pktInfo.getSource(),
                                          pktInfo.getPriority(),
                                          pkt.length(),
                                          PacketStatusPublisher::OutboundAction::DROP_FLOW_CONTROL);

          // drop
          return;
        }
    }

  std::uint8_t u8Queue{priorityToQueue(pkt.getPacketInfo().getPriority())};

  size_t packetsDropped{pQueueManager_->enqueue(u8Queue,std::move(pkt))};

  // drop, replace token
  if(bFlowControlEnable_)
    {
      for(size_t i = 0; i < packetsDropped; ++i)
        {
          auto status = flowControlManager_.addToken();

          if(!status.second)
            {
              LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                      ERROR_LEVEL,
                                      "MACI %03hu TDMA::BaseModel:::%s: failed to add token (tokens:%hu)",
                                      id_,
                                      __func__,
                                      status.first);
            }
        }
    }
}


void EMANE::Models::TDMA::BaseModel::Implementation::processEvent(const EventId & eventId,
                                                                  const Serialization & serialization)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);

  pScheduler_->processEvent(eventId,serialization);

}

void EMANE::Models::TDMA::BaseModel::Implementation::processTimedEvent(TimerEventId,
                                                                       const TimePoint &,
                                                                       const TimePoint &,
                                                                       const TimePoint &,
                                                                       const void * arg)
{
  auto pCallBack = reinterpret_cast<const std::function<bool()> *>(arg);

  if((*pCallBack)())
    {
      delete pCallBack;
    }
}


void EMANE::Models::TDMA::BaseModel::Implementation::processConfiguration(const ConfigurationUpdate & update)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);


  ConfigurationUpdate schedulerConfiguration;

  for(const auto & item : update)
    {
      if(item.first == "enablepromiscuousmode")
        {
          bool bPromiscuousMode{item.second[0].asBool()};

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu TDMA::BaseModel::%s: %s = %s",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  bPromiscuousMode ? "on" : "off");

          receiveManager_.setPromiscuousMode(bPromiscuousMode);
        }
      else
        {
          schedulerConfiguration.push_back(item);
        }
    }

  pScheduler_->configure(schedulerConfiguration);
}

void EMANE::Models::TDMA::BaseModel::Implementation::notifyScheduleChange(const Frequencies & frequencies,
                                                                          std::uint64_t u64BandwidthHz,
                                                                          const Microseconds & slotDuration,
                                                                          const Microseconds & slotOverhead)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);


  if(transmitTimedEventId_)
    {
      pPlatformService_->timerService().cancelTimedEvent(transmitTimedEventId_);
      transmitTimedEventId_ = 0;
    }

  if(u64BandwidthHz_ != u64BandwidthHz || frequencies != frequencies_)
    {
      // only required if freq set/bandwidth differs from existing
      pRadioModel_->sendDownstreamControl({Controls::FrequencyOfInterestControlMessage::create(u64BandwidthHz,frequencies)});

      frequencies_ = frequencies;

      u64BandwidthHz_ = u64BandwidthHz;
    }

  slotDuration_ = slotDuration;

  slotOverhead_ = slotOverhead;

  slotStatusTablePublisher_.clear();

  std::tie(txSlotInfos_,
           nextMultiFrameTime_) =
    pScheduler_->getTxSlotInfo(Clock::now(),1);

  LOGGER_VERBOSE_LOGGING_FN_VARGS(pPlatformService_->logService(),
                                  DEBUG_LEVEL,
                                  TxSlotInfosFormatter(txSlotInfos_),
                                  "MACI %03hu TDMA::BaseModel::%s TX Slot Info",
                                  id_,
                                  __func__);



  if(!txSlotInfos_.empty())
    {
      pendingTxSlotInfo_ = *txSlotInfos_.begin();

      txSlotInfos_.pop_front();

      transmitTimedEventId_ =
        pPlatformService_->timerService().
        scheduleTimedEvent(pendingTxSlotInfo_.timePoint_,
                           new std::function<bool()>{std::bind(&Implementation::processTxOpportunity,
                                                               this)});
    }
}


void EMANE::Models::TDMA::BaseModel::Implementation::processSchedulerPacket(DownstreamPacket & pkt)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);

  // enqueue into max priority queue
  pQueueManager_->enqueue(4,std::move(pkt));
}


void EMANE::Models::TDMA::BaseModel::Implementation::processSchedulerControl(const ControlMessages & msgs)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);

  pRadioModel_->sendDownstreamControl(msgs);
}


EMANE::Models::TDMA::QueueInfos EMANE::Models::TDMA::BaseModel::Implementation::getPacketQueueInfo() const
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu TDMA::BaseModel::%s",
                          id_,
                          __func__);

  return pQueueManager_->getPacketQueueInfo();
}

void EMANE::Models::TDMA::BaseModel::Implementation::sendDownstreamPacket(double dSlotRemainingRatio)
{
  // calculate the number of bytes allowed in the slot
  size_t bytesAvailable =
    (slotDuration_.count() - slotOverhead_.count()) / 1000000.0 * pendingTxSlotInfo_.u64DataRatebps_ / 8.0;

  auto entry = pQueueManager_->dequeue(pendingTxSlotInfo_.u8QueueId_,
                                       bytesAvailable,
                                       pendingTxSlotInfo_.destination_);

  MessageComponents & components{std::get<0>(entry)};
  size_t totalSize{std::get<1>(entry)};

  if(totalSize)
    {
      if(totalSize <= bytesAvailable)
        {
          float fSeconds{totalSize * 8.0f / pendingTxSlotInfo_.u64DataRatebps_};

          Microseconds duration{std::chrono::duration_cast<Microseconds>(DoubleSeconds{fSeconds})};

          NEMId dst{};
          size_t completedPackets{};

          // determine how many components represent completed packets (no fragments remain) and
          // whether to use a unicast or broadcast nem address
          for(const auto & component : components)
            {
              completedPackets += !component.isMoreFragments();

              // if not set, set a destination
              if(!dst)
                {
                  dst = component.getDestination();
                }
              else if(dst != NEM_BROADCAST_MAC_ADDRESS)
                {
                  // if the destination is not broadcast, check to see if it matches
                  // the destination of the current component - if not, set the NEM
                  // broadcast address as the dst
                  if(dst != component.getDestination())
                    {
                      dst = NEM_BROADCAST_MAC_ADDRESS;
                    }
                }
            }


          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  DEBUG_LEVEL,
                                  "MACI %03hu TDMA::BaseModel::%s sending downstream to %03hu components: %zu",
                                  id_,
                                  __func__,
                                  dst,
                                  components.size());


          if(bFlowControlEnable_ && completedPackets)
            {
              auto status = flowControlManager_.addToken(completedPackets);

              if(!status.second)
                {
                  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                          ERROR_LEVEL,
                                          "MACI %03hu TDMA::BaseModel::%s: failed to add token (tokens:%hu)",
                                          id_,
                                          __func__,
                                          status.first);

                }
            }


          BaseModelMessage baseModelMessage{pendingTxSlotInfo_.u64AbsoluteSlotIndex_,
              pendingTxSlotInfo_.u64DataRatebps_,
              std::move(components)};

          Serialization serialization{baseModelMessage.serialize()};

          DownstreamPacket pkt({id_,dst,0,Clock::now()},serialization.c_str(),serialization.size());

          pkt.prependLengthPrefixFraming(serialization.size());

          pRadioModel_->sendDownstreamPacket(CommonMACHeader{REGISTERED_EMANE_MAC_TDMA,u64SequenceNumber_++},
                                             pkt,
                                             {Controls::FrequencyControlMessage::create(
                                                                                        u64BandwidthHz_,
                                                                                        {{pendingTxSlotInfo_.u64FrequencyHz_,duration}}),
                                                 Controls::TimeStampControlMessage::create(pendingTxSlotInfo_.timePoint_)});



          slotStatusTablePublisher_.update(pendingTxSlotInfo_.u32RelativeIndex_,
                                           pendingTxSlotInfo_.u32RelativeFrameIndex_,
                                           pendingTxSlotInfo_.u32RelativeSlotIndex_,
                                           SlotStatusTablePublisher::Status::TX_GOOD,
                                           dSlotRemainingRatio);
        }
      else
        {
          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  ERROR_LEVEL,
                                  "MACI %03hu TDMA::BaseModel::%s queue dequeue returning %zu bytes than slot has available %zu",
                                  id_,
                                  __func__,
                                  totalSize,
                                  bytesAvailable);
        }
    }
}

bool  EMANE::Models::TDMA::BaseModel::Implementation::processTxOpportunity()
{
  auto now = Clock::now();

  Microseconds timeRemainingInSlot{slotDuration_ -
      std::chrono::duration_cast<Microseconds>(now -
                                               pendingTxSlotInfo_.timePoint_)};

  double dSlotRemainingRatio =
    timeRemainingInSlot.count() / static_cast<double>(slotDuration_.count());

  if(timeRemainingInSlot > Microseconds::zero())
    {
      // transmit in this slot
      sendDownstreamPacket(dSlotRemainingRatio);
    }
  else
    {
      slotStatusTablePublisher_.update(pendingTxSlotInfo_.u32RelativeIndex_,
                                       pendingTxSlotInfo_.u32RelativeFrameIndex_,
                                       pendingTxSlotInfo_.u32RelativeSlotIndex_,
                                       SlotStatusTablePublisher::Status::TX_MISSED,
                                       dSlotRemainingRatio);
    }


  // if necessary request more tx opportunities
  if(txSlotInfos_.empty())
    {
      // request more slots
      std::tie(txSlotInfos_,
               nextMultiFrameTime_) =
        pScheduler_->getTxSlotInfo(nextMultiFrameTime_,1);
    }

  bool bFoundTXSlot = {};

  // find the next transmit opportunity
  while(!txSlotInfos_.empty() && !bFoundTXSlot)
    {
      // it might be necessary to request more opportunies if we
      // are behind and have many tx opportunities that have past
      while(!txSlotInfos_.empty())
        {
          pendingTxSlotInfo_ = *txSlotInfos_.begin();

          txSlotInfos_.pop_front();

          if(pendingTxSlotInfo_.timePoint_ > now)
            {
              transmitTimedEventId_ =
                pPlatformService_->timerService().
                scheduleTimedEvent(pendingTxSlotInfo_.timePoint_,
                                   new std::function<bool()>{std::bind(&Implementation::processTxOpportunity,
                                                                       this)});


              bFoundTXSlot = true;
              break;
            }
          else if(pendingTxSlotInfo_.timePoint_ + slotDuration_ <= now)
            {
              double dSlotRemainingRatio = \
                std::chrono::duration_cast<Microseconds>(pendingTxSlotInfo_.timePoint_ - now).count() /
                static_cast<double>(slotDuration_.count());

              // blown tx opportunity
              slotStatusTablePublisher_.update(pendingTxSlotInfo_.u32RelativeIndex_,
                                               pendingTxSlotInfo_.u32RelativeFrameIndex_,
                                               pendingTxSlotInfo_.u32RelativeSlotIndex_,
                                               SlotStatusTablePublisher::Status::TX_MISSED,
                                               dSlotRemainingRatio);
            }
          else
            {
              // send the packet
              timeRemainingInSlot = slotDuration_ -
                std::chrono::duration_cast<Microseconds>(now -
                                                         pendingTxSlotInfo_.timePoint_);


              double dSlotRemainingRatio =
                timeRemainingInSlot.count() / static_cast<double>(slotDuration_.count());


              sendDownstreamPacket(dSlotRemainingRatio);
            }
        }

      // if we are out of slots
      if(txSlotInfos_.empty())
        {
          // request more slots
          std::tie(txSlotInfos_,
                   nextMultiFrameTime_) =
            pScheduler_->getTxSlotInfo(nextMultiFrameTime_,1);
        }
    }

  return false;
}
