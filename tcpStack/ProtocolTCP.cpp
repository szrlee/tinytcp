//----------------------------------------------------------------------------
// Copyright( c ) 2015, Robert Kimball
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//----------------------------------------------------------------------------

#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "ProtocolTCP.h"
#include "ProtocolIPv4.h"
#include "FCS.h"
#include "Utility.h"
#include "DataBuffer.h"
#include "osMutex.h"
#include "osTime.h"

//============================================================================
//
//============================================================================

ProtocolTCP::ProtocolTCP( ProtocolIPv4& ip ) :
   IP( ip )
{
   for( int i=0; i<TCP_MAX_CONNECTIONS; i++ )
   {
      ConnectionList[ i ].Initialize( ip, *this );
   }
}

//============================================================================
//
//============================================================================

void ProtocolTCP::ProcessRx( DataBuffer* rxBuffer, const uint8_t* sourceIP, const uint8_t* targetIP )
{
   TCPConnection* connection;
   uint16_t checksum;
   uint16_t localPort;
   uint16_t remotePort;
   uint8_t headerLength;
   uint8_t* data;
   uint16_t dataLength;
   int count;
   DataBuffer* buffer;
   uint8_t flags = 0;
   uint8_t* packet = rxBuffer->Packet;
   uint16_t length = rxBuffer->Length;
   uint16_t remoteWindowSize;
   uint32_t time_us;

   uint32_t SequenceNumber;
   uint32_t AcknowledgementNumber;

   checksum = ComputeChecksum( packet, length, sourceIP, targetIP );

   if( checksum == 0 )
   {
      // pass
      remotePort            = Unpack16( packet, 0 );
      localPort             = Unpack16( packet, 2 );
      SequenceNumber        = Unpack32( packet, 4 );
      AcknowledgementNumber = Unpack32( packet, 8 );
      headerLength          = (Unpack8( packet, 12 ) >> 4) * 4;
      remoteWindowSize      = Unpack16( packet, 14 );

      rxBuffer->Packet += headerLength;
      rxBuffer->Length -= headerLength;

      connection = LocateConnection( remotePort, sourceIP, localPort );
      if( connection == 0 )
      {
         // No connection found
         printf( "Connection port %d not found\n", localPort );
      }
      else
      {
         // Existing connection, process the state machine
         switch( connection->State )
         {
         case TCPConnection::CLOSED:
            // Do nothing
            Reset( rxBuffer->MAC, localPort, remotePort, sourceIP );
            break;
         case TCPConnection::LISTEN:
            if( SYN )
            {
               // Need a closed connection to work with
               TCPConnection* tmp = NewClient( rxBuffer->MAC, sourceIP, remotePort, localPort );
               if( tmp != 0 )
               {
                  tmp->Parent = connection;
                  connection = tmp;
                  connection->State = TCPConnection::SYN_RECEIVED;
                  connection->AcknowledgementNumber = SequenceNumber;
                  connection->LastAck = connection->AcknowledgementNumber;
                  connection->AcknowledgementNumber++;   // SYN flag consumes a sequence number
                  connection->SendFlags( FLAG_SYN | FLAG_ACK );
                  connection->SequenceNumber++; // Our SYN costs too
               }
               else
               {
                  printf( "Failed to get connection for SYN\n" );
               }
            }
            break;
         case TCPConnection::SYN_SENT:
            if( SYN )
            {
               connection->AcknowledgementNumber = SequenceNumber;
               connection->LastAck = connection->AcknowledgementNumber;
               if( ACK )
               {
                  connection->State = TCPConnection::ESTABLISHED;
                  connection->SendFlags( FLAG_ACK );
               }
               else
               {
                  // Simultaneous open
                  connection->State = TCPConnection::SYN_RECEIVED;
                  connection->AcknowledgementNumber++;   // SYN flag consumes a sequence number
                  connection->SendFlags( FLAG_SYN | FLAG_ACK );
               }
            }
            break;
         case TCPConnection::SYN_RECEIVED:
            if( ACK )
            {
               connection->State = TCPConnection::ESTABLISHED;

               if( connection->Parent->NewConnection == 0 )
               {
                  connection->MaxSequenceTx = AcknowledgementNumber + remoteWindowSize;
                  connection->Parent->NewConnection = connection;
                  connection->Parent->Event.Notify();
               }
            }
            break;
         case TCPConnection::ESTABLISHED:
            if( FIN )
            {
               connection->State = TCPConnection::CLOSE_WAIT;
               connection->AcknowledgementNumber++;      // FIN consumes sequence number
               connection->SendFlags( FLAG_ACK );
            }
            break;
         case TCPConnection::FIN_WAIT_1:
            if( FIN )
            {
               if( ACK )
               {
                  connection->State = TCPConnection::TIMED_WAIT;
                  // Start TimedWait timer
               }
               else
               {
                  connection->State = TCPConnection::CLOSING;
               }
               connection->AcknowledgementNumber++;      // FIN consumes sequence number
               connection->SendFlags( FLAG_ACK );
            }
            else if( ACK )
            {
               connection->State = TCPConnection::FIN_WAIT_2;
            }
            break;
         case TCPConnection::FIN_WAIT_2:
            if( FIN )
            {
               connection->State = TCPConnection::TIMED_WAIT;
               // Start TimedWait timer
               connection->AcknowledgementNumber++;      // FIN consumes sequence number
               connection->Time_us = (int32_t)osTime::GetTime();
               connection->SendFlags( FLAG_ACK );
            }
            break;
         case TCPConnection::CLOSE_WAIT:
            break;
         case TCPConnection::CLOSING:
            break;
         case TCPConnection::LAST_ACK:
            if( ACK )
            {
               connection->State = TCPConnection::CLOSED;
            }
            break;
         case TCPConnection::TIMED_WAIT:
            break;
         default:
            break;
         }

         // Handle any data received
         if
         (
            connection &&
            connection->State == TCPConnection::ESTABLISHED ||
            connection->State == TCPConnection::FIN_WAIT_1 ||
            connection->State == TCPConnection::FIN_WAIT_2 ||
            connection->State == TCPConnection::CLOSE_WAIT
         )
         {
            data = rxBuffer->Packet;
            dataLength = rxBuffer->Length;

            connection->MaxSequenceTx = AcknowledgementNumber + remoteWindowSize;
            connection->Event.Notify();

            // Handle any ACKed data
            if( ACK )
            {
               connection->HoldingQueueLock.Take( __FILE__, __LINE__ );
               count = connection->HoldingQueue.GetCount();
               time_us = (uint32_t)osTime::GetTime();
               for( int i=0; i<count; i++ )
               {
                  buffer = (DataBuffer*)connection->HoldingQueue.Get();
                  if( (int32_t)(AcknowledgementNumber - buffer->AcknowledgementNumber) >= 0 )
                  {
                     connection->CalculateRTT( (int32_t)(time_us - buffer->Time_us) );
                     IP.FreeTxBuffer( buffer );
                  }
                  else
                  {
                     connection->HoldingQueue.Put( buffer );
                  }
               }
               connection->HoldingQueueLock.Give();
            }

            if( FIN )
            {
               if( connection->State == TCPConnection::FIN_WAIT_1 )
               {
                  flags |= FLAG_ACK;
                  connection->State = TCPConnection::CLOSE_WAIT;
               }
               else if( connection->State == TCPConnection::ESTABLISHED )
               {
                  connection->State = TCPConnection::CLOSE_WAIT;
                  flags |= FLAG_ACK;
               }
            }

            // ACK the receipt of the data
            if( dataLength > 0 )
            {
               // Copy it to the application
               rxBuffer->Disposable = false;
               connection->StoreRxData( rxBuffer );
               IP.FreeRxBuffer( rxBuffer );
               connection->Event.Notify();
            }

            if( flags != 0 )
            {
               connection->SendFlags( flags );
            }
         }
      }
   }
   else
   {
      printf( "TCP Checksum Failure\n" );
   }
}

//============================================================================
//
//============================================================================

void ProtocolTCP::Reset( InterfaceMAC* mac, uint16_t localPort, uint16_t remotePort, const uint8_t* remoteAddress )
{
   uint8_t* packet;
   uint16_t checksum;
   uint16_t length;

   DataBuffer* buffer = IP.GetTxBuffer( mac );

   if( buffer == 0 )
   {
      return;
   }

   buffer->Packet    += TCP_HEADER_SIZE;
   buffer->Remainder -= TCP_HEADER_SIZE;

   buffer->Packet -= TCP_HEADER_SIZE;
   packet = buffer->Packet;
   length = buffer->Length;
   if( packet != 0 )
   {
      Pack16( packet, 0, localPort );
      Pack16( packet, 2, remotePort );
      Pack32( packet, 4, 0 );       // Sequence
      Pack32( packet, 8, 0 );       // AckSequence
      Pack8( packet, 12, 0x50 );    // Header length and reserved
      Pack8( packet, 13, FLAG_RST );
      Pack16( packet, 14, 0 );      // window size
      Pack16( packet, 16, 0 );      // clear checksum
      Pack16( packet, 18, 0 );      // 2 bytes of UrgentPointer

      checksum = ProtocolTCP::ComputeChecksum( packet, TCP_HEADER_SIZE, IP.GetUnicastAddress(), remoteAddress );

      Pack16( packet, 16, checksum ); // checksum

      buffer->Length += TCP_HEADER_SIZE;
      buffer->Remainder -= buffer->Length;

      IP.Transmit( buffer, 0x06, remoteAddress, IP.GetUnicastAddress() );
   }
}

//============================================================================
//
//============================================================================

uint16_t ProtocolTCP::ComputeChecksum( uint8_t* packet, uint16_t length, const uint8_t* sourceIP, const uint8_t* targetIP )
{
   uint32_t checksum;
   uint16_t tmp;

   // A whole lot o' hokum just to compute the checksum
   checksum = FCS::ChecksumAdd( sourceIP, 4, 0 );
   checksum = FCS::ChecksumAdd( targetIP, 4, checksum );
   checksum += 0x06;    // protocol
   checksum += length;
   if( (length & 0x0001) != 0 )
   {
      // length is odd
      tmp = length+1;
      packet[ length ] = 0;
   }
   else
   {
      tmp = length;
   }
   checksum = FCS::ChecksumAdd( packet, tmp, checksum );

   return FCS::ChecksumComplete( checksum );
}

//============================================================================
//
//============================================================================

TCPConnection* ProtocolTCP::LocateConnection
(
   uint16_t remotePort,
   const uint8_t* remoteAddress,
   uint16_t localPort
)
{
   int i;

   // Must do two passes:
   // First pass to look for established connections
   // Second pass to look for listening connections

   // Pass 1
   for( i=0; i<TCP_MAX_CONNECTIONS; i++ )
   {
      //printf( "%s %d: connection port %d, state %s\n", FindFileName( __FILE__ ), __LINE__, ConnectionList[ i ].LocalPort, ConnectionList[ i ].GetStateString() );
      if
      (
         ConnectionList[ i ].LocalPort == localPort &&
         ConnectionList[ i ].RemotePort == remotePort &&
         AddressCompare( ConnectionList[ i ].RemoteAddress, remoteAddress, IP.AddressSize() )
      )
      {
         return &ConnectionList[ i ];
      }
   }

   // Pass 2
   for( i=0; i<TCP_MAX_CONNECTIONS; i++ )
   {
      if( ConnectionList[ i ].LocalPort == localPort && ConnectionList[ i ].State == TCPConnection::LISTEN )
      {
         return &ConnectionList[ i ];
      }
   }

   return 0;
}

//============================================================================
//
//============================================================================

uint16_t ProtocolTCP::NewPort()
{
   int i;

   if( NextPort <= 1024 )
   {
      NextPort = 1024;
   }

   NextPort++;

   for( i=0; i<TCP_MAX_CONNECTIONS; i++ )
   {
      if( ConnectionList[ i ].LocalPort == NextPort )
      {
         NextPort++;
         i=-1;
      }
   }

   return NextPort;
}

//============================================================================
//
//============================================================================

TCPConnection* ProtocolTCP::NewClient
(
   InterfaceMAC* mac,
   const uint8_t* remoteAddress,
   uint16_t remotePort,
   uint16_t localPort
)
{
   int i;
   int j;

   for( i=0; i<TCP_MAX_CONNECTIONS; i++ )
   {
      TCPConnection& connection = ConnectionList[ i ];
      if( connection.State == TCPConnection::CLOSED )
      {
         connection.LocalPort = localPort;
         connection.SequenceNumber = 1;
         connection.MaxSequenceTx = connection.SequenceNumber + 1024;
         for( j=0; j<IP.AddressSize(); j++ )
         {
            connection.RemoteAddress[ j ] = remoteAddress[ j ];
         }
         connection.RemotePort = remotePort;
         connection.MAC = mac;

         return &connection;
      }
   }

   return 0;
}

//============================================================================
//
//============================================================================

TCPConnection* ProtocolTCP::NewServer( InterfaceMAC* mac, uint16_t port )
{
   int i;

   for( i=0; i<TCP_MAX_CONNECTIONS; i++ )
   {
      TCPConnection& connection = ConnectionList[ i ];
      if( connection.State == TCPConnection::CLOSED )
      {
         connection.State = TCPConnection::LISTEN;
         connection.LocalPort = port;
         connection.MAC = mac;
         return &connection;
      }
   }

   return 0;
}

//============================================================================
//
//============================================================================

void ProtocolTCP::Tick()
{
   int i;

   for( i=0; i<TCP_MAX_CONNECTIONS; i++ )
   {
      if
      (
         ConnectionList[ i ].State == TCPConnection::ESTABLISHED ||
         ConnectionList[ i ].State == TCPConnection::TIMED_WAIT
      )
      {
         ConnectionList[ i ].Tick();
      }
   }
}

//============================================================================
//
//============================================================================

void ProtocolTCP::Show( osPrintfInterface* out )
{
   out->Printf( "TCP Information\n" );
   for( int i = 0; i<TCP_MAX_CONNECTIONS; i++ )
   {
      out->Printf( "connection %s   ", ConnectionList[ i ].GetStateString() );
      switch( ConnectionList[ i ].State )
      {
      case TCPConnection::LISTEN:
         out->Printf( "     local=%d  ", ConnectionList[ i ].LocalPort );
         break;
      case TCPConnection::ESTABLISHED:
         out->Printf( "local=%d  remote=%d.%d.%d.%d:%d", ConnectionList[ i ].LocalPort, ConnectionList[ i ].RemoteAddress[0], ConnectionList[ i ].RemoteAddress[ 1 ], ConnectionList[ i ].RemoteAddress[ 2 ], ConnectionList[ i ].RemoteAddress[ 3 ], ConnectionList[ i ].RemotePort );
         break;
      default:
         break;
      }
      out->Printf( "\n" );
   }
}
