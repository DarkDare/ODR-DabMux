/*
   Copyright (C) 2009 Her Majesty the Queen in Right of Canada (Communications
   Research Center Canada)

   Copyright (C) 2013 Matthias P. Braendli
   http://mpb.li

   ZeroMQ input. see www.zeromq.org for more info

   From the ZeroMQ manpage 'zmq':

       The 0MQ lightweight messaging kernel is a library which extends the standard
       socket interfaces with features traditionally provided by specialised
       messaging middleware products. 0MQ sockets provide an abstraction of
       asynchronous message queues, multiple messaging patterns, message filtering
       (subscriptions), seamless access to multiple transport protocols and more.
   */
/*
   This file is part of CRC-DabMux.

   CRC-DabMux is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   CRC-DabMux is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with CRC-DabMux.  If not, see <http://www.gnu.org/licenses/>.
   */

#include "dabInput.h"
#include "dabInputZmq.h"
#include "StatsServer.h"

#include <stdio.h>
#include <zmq.hpp>
#include <list>
#include <exception>
#include <string.h>
#include <string>
#include <sstream>
#include <limits.h>

#ifdef __MINGW32__
#   define bzero(s, n) memset(s, 0, n)
#endif

#ifdef HAVE_INPUT_ZEROMQ

extern StatsServer global_stats;

int DabInputZmq::open(const std::string inputUri)
{
    // Prepare the ZMQ socket to accept connections
    try {
        m_zmq_sock.bind(inputUri.c_str());
    }
    catch (zmq::error_t& err) {
        std::ostringstream os;
        os << "ZMQ bind for input " << m_name << " failed";
        throw std::runtime_error(os.str());
    }

    try {
        m_zmq_sock.setsockopt(ZMQ_SUBSCRIBE, NULL, 0);
    }
    catch (zmq::error_t& err) {
        std::ostringstream os;
        os << "ZMQ set socket options for input " << m_name << " failed";
        throw std::runtime_error(os.str());
    }

    // We want to appear in the statistics !
    global_stats.registerInput(m_name);

    return 0;
}

// size corresponds to a frame size. It is constant for a given bitrate
int DabInputZmq::readFrame(void* buffer, int size)
{
    int rc;

    /* We must *always* read data from the ZMQ socket,
     * to make sure that ZMQ internal buffers are emptied
     * quickly. It's the only way to control the buffers
     * of the whole path from encoder to our frame_buffer.
     */
    rc = readFromSocket(size);

    /* Notify of a buffer overrun, and drop some frames */
    if (m_frame_buffer.size() >= INPUT_ZMQ_MAX_BUFFER_SIZE) {
        global_stats.notifyOverrun(m_name);

        /* If the buffer is really too full, we drop as many frames as needed
         * to get down to the prebuffering size. We would like to have our buffer
         * filled to the prebuffering length.
         */
        if (m_frame_buffer.size() >= 1.5*INPUT_ZMQ_MAX_BUFFER_SIZE) {
            size_t over_max = m_frame_buffer.size() - INPUT_ZMQ_PREBUFFERING;

            while (over_max--) {
                m_frame_buffer.pop_front();
            }
        }
        else {
            /* Our frame_buffer contains DAB logical frames. Five of these make one
             * AAC superframe.
             *
             * Dropping this superframe amounts to dropping 120ms of audio.
             *
             * We're actually not sure to drop five DAB logical frames
             * beloning to the same AAC superframe. It is assumed that no
             * receiver will crash because of this. At least, the DAB logical frame
             * vs. AAC superframe alignment is preserved.
             *
             * TODO: of course this assumption probably doesn't hold. Fix this !
             * */
            m_frame_buffer.pop_front();
            m_frame_buffer.pop_front();
            m_frame_buffer.pop_front();
            m_frame_buffer.pop_front();
            m_frame_buffer.pop_front();
        }
    }

    if (m_prebuffering > 0) {
        if (rc > 0)
            m_prebuffering--;
        if (m_prebuffering == 0)
            etiLog.log(info, "inputZMQ %s input pre-buffering complete\n",
                m_name.c_str());

        /* During prebuffering, give a zeroed frame to the mux */
        global_stats.notifyUnderrun(m_name);
        memset(buffer, 0, size);
        return size;
    }

    // Save stats data in bytes, not in frames
    global_stats.notifyBuffer(m_name, m_frame_buffer.size() * size);

    if (m_frame_buffer.empty()) {
        etiLog.log(warn, "inputZMQ %s input empty, re-enabling pre-buffering\n",
                m_name.c_str());
        // reset prebuffering
        m_prebuffering = INPUT_ZMQ_PREBUFFERING;

        /* We have no data to give, we give a zeroed frame */
        global_stats.notifyUnderrun(m_name);
        memset(buffer, 0, size);
        return size;
    }
    else
    {
        /* Normal situation, give a frame from the frame_buffer */
        char* newframe = m_frame_buffer.front();
        memcpy(buffer, newframe, size);
        delete[] newframe;
        m_frame_buffer.pop_front();
        return size;
    }
}

// Read a superframe from the socket, cut it into five frames, and push to list
int DabInputZmq::readFromSocket(int framesize)
{
    int rc;
    int nBytes;
    zmq::message_t msg;

    try {
        nBytes = m_zmq_sock.recv(&msg, ZMQ_DONTWAIT);
        if (nBytes == 0) {
            return 0;
        }
    }
    catch (zmq::error_t& err)
    {
        etiLog.level(error) << "Failed to receive from zmq socket " <<
                m_name << ": " << err.what();
    }

    char* data = (char*)msg.data();

    /* TS 102 563, Section 6:
     * Audio super frames are transported in five successive DAB logical frames
     * with additional error protection.
     */
    if (nBytes == 5*framesize)
    {
        if (m_frame_buffer.size() > INPUT_ZMQ_MAX_BUFFER_SIZE) {
            etiLog.level(warn) <<
                "inputZMQ " << m_name <<
                " buffer full (" << m_frame_buffer.size() << "),"
                " dropping incoming superframe !";
            nBytes = 0;
        }
        else {
            // copy the input frame blockwise into the frame_buffer
            for (char* framestart = data;
                    framestart < &data[5*framesize];
                    framestart += framesize) {
                char* frame = new char[framesize];
                memcpy(frame, framestart, framesize);
                m_frame_buffer.push_back(frame);
            }
        }
    }
    else
    {
        etiLog.level(error) <<
            "inputZMQ " << m_name <<
            " wrong data size: recv'd " << nBytes <<
            ", need " << 5*framesize << ".";
        nBytes = 0;
    }

    return nBytes;
}

int DabInputZmq::close()
{
    m_zmq_sock.close();
    return 0;
}

int DabInputZmq::setBitrate(int bitrate)
{
    m_bitrate = bitrate;
    return bitrate; // TODO do a nice check here
}

#endif
