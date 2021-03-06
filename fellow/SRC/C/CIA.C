/*=========================================================================*/
/* Fellow                                                                  */
/* Cia emulation                                                           */
/*                                                                         */
/* Authors: Petter Schau                                                   */
/*          Rainer Sinsch                                                  */
/*          Marco Nova (novamarco@hotmail.com)                             */
/*                                                                         */
/*                                                                         */
/* Copyright (C) 1991, 1992, 1996 Free Software Foundation, Inc.           */
/*                                                                         */
/* This program is free software; you can redistribute it and/or modify    */
/* it under the terms of the GNU General Public License as published by    */
/* the Free Software Foundation; either version 2, or (at your option)     */
/* any later version.                                                      */
/*                                                                         */
/* This program is distributed in the hope that it will be useful,         */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of          */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           */
/* GNU General Public License for more details.                            */
/*                                                                         */
/* You should have received a copy of the GNU General Public License       */
/* along with this program; if not, write to the Free Software Foundation, */
/* Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.          */
/*=========================================================================*/

#include "defs.h"
#include "bus.h"
#include "gameport.h"
#include "fmem.h"
#include "floppy.h"
#include "cia.h"
#include "interrupt.h"
#include "fellow/api/Services.h"

using namespace fellow::api;

#define CIA_NO_EVENT          0
#define CIAA_TA_TIMEOUT_EVENT 1
#define CIAA_TB_TIMEOUT_EVENT 2
#define CIAB_TA_TIMEOUT_EVENT 3
#define CIAB_TB_TIMEOUT_EVENT 4
#define CIA_RECHECK_IRQ_EVENT 5

#define CIA_TA_IRQ    1
#define CIA_TB_IRQ    2
#define CIA_ALARM_IRQ 4
#define CIA_KBD_IRQ   8
#define CIA_FLAG_IRQ  16

#define CIA_BUS_CYCLE_RATIO 5

//#define CIA_LOGGING

#ifdef CIA_LOGGING
extern ULO cpuGetOriginalPC();
#endif

typedef UBY (*ciaFetchFunc)(ULO i);
typedef void (*ciaWriteFunc)(ULO i, UBY data);

ULO cia_next_event_type; /* What type of event */

/* Cia registers, index 0 is Cia A, index 1 is Cia B */

typedef struct cia_state_
{
  ULO ta;
  ULO tb;              
  ULO ta_rem; /* Preserves remainder when downsizing from bus cycles */
  ULO tb_rem;              
  ULO talatch;
  ULO tblatch;          
  LON taleft;
  LON tbleft;
  ULO evalarm;          
  ULO evlatch;               
  ULO evlatching;
  ULO evwritelatch;               
  ULO evwritelatching;               
  ULO evalarmlatch;
  ULO evalarmlatching;
  ULO ev;
  UBY icrreq;
  UBY icrmsk;           
  UBY cra;              
  UBY crb;              
  UBY pra;
  UBY prb;              
  UBY ddra;             
  UBY ddrb;             
  UBY sp;
} cia_state;

cia_state cia[2];

bool cia_recheck_irq;
ULO cia_recheck_irq_time;

BOOLE ciaIsSoundFilterEnabled(void)
{
  return (cia[0].pra & 2) == 2;
}

/* Translate timer -> cycles until timeout from current sof, start of frame */

ULO ciaUnstabilizeValue(ULO value, ULO remainder) {
  return (value*CIA_BUS_CYCLE_RATIO) + bus.cycle + remainder;
}

/* Translate cycles until timeout from current sof -> timer value */

ULO ciaStabilizeValue(ULO value) {
  return (value - bus.cycle) / CIA_BUS_CYCLE_RATIO;
}

ULO ciaStabilizeValueRemainder(ULO value) {
  return (value - bus.cycle) % CIA_BUS_CYCLE_RATIO;
}

ULO ciaGetTimerValue(ULO value)
{
  if (value == 0)
  {
#ifdef CIA_LOGGING  
    Service->Log.AddLogDebug("CIA Warning timer latch is zero. PC %X", cpuGetOriginalPC());
#endif    
    return 1; // Avoid getting stuck on zero timeout.
  }
  return value;
}

void ciaTAStabilize(ULO i)
{
  if (cia[i].cra & 1)
  {
    cia[i].ta = ciaStabilizeValue(cia[i].taleft);
    cia[i].ta_rem = ciaStabilizeValueRemainder(cia[i].taleft);
  }
  cia[i].taleft = BUS_CYCLE_DISABLE;
}

void ciaTBStabilize(ULO i)
{
  if ((cia[i].crb & 0x41) == 1)
  { // Timer B started and not attached to timer A
    cia[i].tb = ciaStabilizeValue(cia[i].tbleft);
    cia[i].tb_rem = ciaStabilizeValueRemainder(cia[i].tbleft);
  }
  cia[i].tbleft = BUS_CYCLE_DISABLE;
}

void ciaStabilize(ULO i)
{
  ciaTAStabilize(i);
  ciaTBStabilize(i);
}

void ciaTAUnstabilize(ULO i)
{
  if (cia[i].cra & 1)
    cia[i].taleft = ciaUnstabilizeValue(cia[i].ta, cia[i].ta_rem);
}

void ciaTBUnstabilize(ULO i)
{
  if ((cia[i].crb & 0x41) == 1) // Timer B started and not attached to timer A
    cia[i].tbleft = ciaUnstabilizeValue(cia[i].tb, cia[i].tb_rem);
}

void ciaUnstabilize(ULO i) 
{
  ciaTAUnstabilize(i);
  ciaTBUnstabilize(i);
}  

void ciaUpdateIRQ(ULO i)
{
  if (cia[i].icrreq & cia[i].icrmsk)
  {
#ifdef CIA_LOGGING 
    Service->Log.AddLogDebug("CIA %c IRQ, req is %X, icrmsk is %X\n", (i == 0) ? 'A' : 'B', cia[i].icrreq, cia[i].icrmsk);
#endif

    cia[i].icrreq |= 0x80;
    UWO mask = (i == 0) ? 0x0008 : 0x2000;
    if (!interruptIsRequested(mask))
    {
      wintreq_direct(mask | 0x8000, 0xdff09c, true);
    }
  }
}

void ciaRaiseIRQ(ULO i, ULO req)
{
  cia[i].icrreq |= (req & 0x1f);
  ciaUpdateIRQ(i);
}

/* Helps the floppy loader, Cia B Flag IRQ */

void ciaRaiseIndexIRQ(void) {
  ciaRaiseIRQ(1, CIA_FLAG_IRQ);
}

void ciaCheckAlarmMatch(ULO i)
{
  if (cia[i].ev == cia[i].evalarm)
  {
#ifdef CIA_LOGGING  
    Service->Log.AddLogDebug("CIA %c Alarm IRQ\n", (i == 0) ? 'A' : 'B');
#endif

    ciaRaiseIRQ(i, CIA_ALARM_IRQ);
  }
}

/* Timeout handlers */

void ciaHandleTBTimeout(ULO i) {
#ifdef CIA_LOGGING  
  Service->Log.AddLogDebug("CIA %c Timer B expired in %s mode, reloading %X\n", (i == 0) ? 'A' : 'B', (cia[i].crb & 8) ? "one-shot" : "continuous", cia[i].tblatch);
#endif

  cia[i].tb = ciaGetTimerValue(cia[i].tblatch);      /* Reload from latch */
    
  if (cia[i].crb & 8)              /* One Shot Mode */
  {            
    cia[i].crb &= 0xfe;            /* Stop timer */
    cia[i].tbleft = BUS_CYCLE_DISABLE;
  }
  else if (!(cia[i].crb & 0x40))   /* Continuous mode, no attach */
  {
    cia[i].tbleft = ciaUnstabilizeValue(cia[i].tb, 0);
  }

  ciaRaiseIRQ(i, CIA_TB_IRQ);     /* Raise irq */
}

void ciaHandleTATimeout(ULO i)
{
#ifdef CIA_LOGGING  
  Service->Log.AddLogDebug("CIA %c Timer A expired in %s mode, reloading %X\n", (i == 0) ? 'A' : 'B', (cia[i].cra & 8) ? "one-shot" : "continuous", cia[i].talatch);
#endif

  cia[i].ta = ciaGetTimerValue(cia[i].talatch);      /* Reload from latch */
  if ((cia[i].crb & 0x41) == 0x41)
  {                                                  /* Timer B attached and started */
    cia[i].tb = (cia[i].tb - 1) & 0xffff;
    if (cia[i].tb == 0)
    {
      ciaHandleTBTimeout(i);
    }
  }
  if (cia[i].cra & 8)              /* One Shot Mode */
  {
    cia[i].cra &= 0xfe;            /* Stop timer */
    cia[i].taleft = BUS_CYCLE_DISABLE;
  }
  else                             /* Continuous mode */
  {
    cia[i].taleft = ciaUnstabilizeValue(cia[i].ta, 0);
  }

#ifdef CIA_LOGGING  
  Service->Log.AddLogDebug("CIA %c Timer A attempt to raise irq, TA icr mask is %d\n", (i == 0) ? 'A' : 'B', cia[i].icrmsk & 1);
#endif
  ciaRaiseIRQ(i, CIA_TA_IRQ);    /* Raise irq */
}

/* Called from eol-handler (B) or eof handler (A) */

void ciaUpdateEventCounter(ULO i)
{
  if (!cia[i].evwritelatching)
  {
    cia[i].ev = (cia[i].ev + 1) & 0xffffff;
    ciaCheckAlarmMatch(i);
  }
}

/* Called from the eof-handler to update timers */

void ciaUpdateTimersEOF()
{
  for (int i = 0; i < 2; i++)
  {
    if (cia[i].taleft >= 0)
    {
      if ((cia[i].taleft -= busGetCyclesInThisFrame()) < 0)
      {
        cia[i].taleft = 0;
      }
    }
    if (cia[i].tbleft >= 0)
    {
      if ((cia[i].tbleft -= busGetCyclesInThisFrame()) < 0)
      {
        cia[i].tbleft = 0;
      }
    }
  }

  if (cia_recheck_irq)
  {
    cia_recheck_irq_time -= busGetCyclesInThisFrame();
  }

  if (ciaEvent.cycle != BUS_CYCLE_DISABLE)
  {
    if (((LON)(ciaEvent.cycle -= busGetCyclesInThisFrame())) < 0)
    {
      ciaEvent.cycle = 0;
    }
    busRemoveEvent(&ciaEvent);
    busInsertEvent(&ciaEvent);
  }

  ciaUpdateEventCounter(0);
}

/* Record next timer timeout */

void ciaEventSetup()
{
  if (ciaEvent.cycle != BUS_CYCLE_DISABLE)
  {
    busInsertEvent(&ciaEvent);
  }
}

void ciaSetupNextEvent()
{
  ULO nextevtime = BUS_CYCLE_DISABLE, nextevtype = CIA_NO_EVENT, i;

  if (cia_recheck_irq)
  {
    nextevtime = cia_recheck_irq_time;
    nextevtype = CIA_RECHECK_IRQ_EVENT;
  }

  for (i = 0; i < 2; i++)
  {
    if (((ULO) cia[i].taleft) < nextevtime)
    {
      nextevtime = cia[i].taleft;
      nextevtype = (i*2) + 1;
    }
    if (((ULO) cia[i].tbleft) < nextevtime)
    {
      nextevtime = cia[i].tbleft;
      nextevtype = (i*2) + 2;
    }
  }

  if (ciaEvent.cycle != BUS_CYCLE_DISABLE)
  {
    busRemoveEvent(&ciaEvent);
    ciaEvent.cycle = BUS_CYCLE_DISABLE;
  }

  ciaEvent.cycle = nextevtime;
  cia_next_event_type = nextevtype;
  ciaEventSetup();
}

void ciaHandleEvent(void)
{
  ciaEvent.cycle = BUS_CYCLE_DISABLE;
  switch (cia_next_event_type)
  {
  case CIAA_TA_TIMEOUT_EVENT:
    ciaHandleTATimeout(0);
    break;
  case CIAA_TB_TIMEOUT_EVENT:
    ciaHandleTBTimeout(0);
    break;
  case CIAB_TA_TIMEOUT_EVENT:
    ciaHandleTATimeout(1);
    break;
  case CIAB_TB_TIMEOUT_EVENT:
    ciaHandleTBTimeout(1);
    break;
  case CIA_RECHECK_IRQ_EVENT:
    cia_recheck_irq = false;
    cia_recheck_irq_time = BUS_CYCLE_DISABLE;
    ciaUpdateIRQ(0);
    ciaUpdateIRQ(1);
    break;
  default:
    break;
  }
  ciaSetupNextEvent();
}

void ciaRecheckIRQ()
{
  cia_recheck_irq = true;
  cia_recheck_irq_time = busGetCycle() + 10;
  ciaSetupNextEvent();
}

/* PRA */

UBY ciaReadApra(void)
{
  UBY result = 0;
  ULO drivesel;

  if( gameport_autofire0[0] )
    gameport_fire0[0] = !gameport_fire0[0];
  if( gameport_autofire0[1] )
    gameport_fire0[1] = !gameport_fire0[1];

  if (!gameport_fire0[0])
    result |= 0x40;	/* Two firebuttons on port 1 */
  if (!gameport_fire0[1])
    result |= 0x80;
  drivesel = floppySelectedGet();       /* Floppy bits */

  if (!floppyIsReady(drivesel))
    result |= 0x20;
  if (!floppyIsTrack0(drivesel))
    result |= 0x10;
  if (!floppyIsWriteProtected(drivesel))
    result |= 8;
  if (!floppyIsChanged(drivesel))
    result |= 4;
  return result | (UBY)(cia[0].pra & 2);
}

UBY ciaReadBpra(void)
{
  return (UBY) cia[1].pra;
}

UBY ciaReadpra(ULO i)
{
  if (i == 0)
    return ciaReadApra();

  return ciaReadBpra();
}

void ciaWriteApra(UBY data)
{
  if ((data & 0x1) && !(cia[0].pra & 0x1))
  {
    memoryChipMap(true);
  }
  else if ((cia[0].pra & 0x1) && !(data & 0x1))
  {
    memoryChipMap(false);
  }
  cia[0].pra = data;
}

void ciaWriteBpra(UBY data)
{
  cia[1].pra = data;
}

void ciaWritepra(ULO i, UBY data)
{
  if (i == 0)
    ciaWriteApra(data);
  else
    ciaWriteBpra(data);
}

/* PRB */

UBY ciaReadprb(ULO i)
{
  return cia[i].prb;
}


void ciaWriteAprb(UBY data)
{
  cia[0].prb = data;
}

/* Motor, drive latches this value when SEL goes from high to low */

void ciaWriteBprb(UBY data)
{
  int j = 0;
  BOOLE motor_was_high = (cia[1].prb & 0x80) == 0x80;
  BOOLE motor_is_high = (data & 0x80) == 0x80;

  for (int i = 8; i < 0x80; i <<= 1, j++)
  {
    BOOLE sel_was_high = cia[1].prb & i;
    BOOLE sel_is_high = data & i;
    if (sel_was_high && !sel_is_high)
    {
      // Motor is latched when sel goes from high to low
      // According to HRM motor bit must be set up in advance by software
      if (!motor_was_high || !motor_is_high)
      {
        floppyMotorSet(j, 0); // 0 is on
      }
      else if (motor_was_high)
      {
        floppyMotorSet(j, 1); // 1 is off
      }
    }
  }
  cia[1].prb = data;
  floppySelectedSet((data & 0x78) >> 3);
  floppySideSet((data & 4) >> 2);
  floppyDirSet((data & 2) >> 1);
  floppyStepSet(data & 1);
}

void ciaWriteprb(ULO i, UBY data)
{
  if (i == 0)
    ciaWriteAprb(data);
  else
    ciaWriteBprb(data);
}

/* DDRA */

UBY ciaReadddra(ULO i)
{
  if (i == 0)
    return 3;
  return 0xff;
}

void ciaWriteddra(ULO i, UBY data)
{
}

/* DDRB */

UBY ciaReadddrb(ULO i)
{
  if (i == 0)
    return cia[0].ddrb;
  return 0xff;
}

void ciaWriteddrb(ULO i, UBY data)
{
  if (i == 0)
    cia[0].ddrb = data;
}

/* SP (Keyboard serial data on Cia A) */

UBY ciaReadsp(ULO i)
{
  return cia[i].sp;
}

void ciaWritesp(ULO i, UBY data)
{
  cia[i].sp = data;
}

/* Timer A */

UBY ciaReadtalo(ULO i)
{
  if (cia[i].cra & 1)
    return (UBY) ciaStabilizeValue(cia[i].taleft);
  return (UBY) cia[i].ta;
}

UBY ciaReadtahi(ULO i)
{
  if (cia[i].cra & 1)
    return (UBY)(ciaStabilizeValue(cia[i].taleft)>>8);
  return (UBY)(cia[i].ta>>8);
}  

void ciaWritetalo(ULO i, UBY data)
{
  cia[i].talatch = (cia[i].talatch & 0xff00) | (ULO)data;

#ifdef CIA_LOGGING  
  Service->Log.AddLogDebug("CIA %c Timer A written (low-part): %X PC %X\n", (i == 0) ? 'A' : 'B', cia[i].talatch, cpuGetOriginalPC());
#endif
}

bool ciaMustReloadOnTHiWrite(UBY cr)
{
  // Reload when not started, or one-shot mode
  return !(cr & 1) || (cr & 8);
}

void ciaWritetahi(ULO i, UBY data)
{
  cia[i].talatch = (cia[i].talatch & 0xff) | (((ULO)data)<<8);

  if (ciaMustReloadOnTHiWrite(cia[i].cra)) // Reload when not started, or one-shot mode
  {
    cia[i].ta = ciaGetTimerValue(cia[i].talatch);
    cia[i].ta_rem = 0;
    cia[i].taleft = BUS_CYCLE_DISABLE;
  }

#ifdef CIA_LOGGING  
  Service->Log.AddLogDebug("CIA %c Timer A written (hi-part): %X PC %X\n", (i == 0) ? 'A' : 'B', cia[i].talatch, cpuGetOriginalPC());
#endif

  if (cia[i].cra & 8) // Timer A is one-shot, write starts it
  {
    cia[i].cra |= 1;
    ciaUnstabilize(i);
    ciaSetupNextEvent();

#ifdef CIA_LOGGING  
    Service->Log.AddLogDebug("CIA %c Timer A one-shot mode automatically started PC %X\n", (i == 0) ? 'A' : 'B', cpuGetOriginalPC());
#endif
  }
}

/* Timer B */

UBY ciaReadtblo(ULO i)
{
  if ((cia[i].crb & 1) && !(cia[i].crb & 0x40))
    return (UBY)ciaStabilizeValue(cia[i].tbleft);
  return (UBY)cia[i].tb;
}

UBY ciaReadtbhi(ULO i)
{
  if ((cia[i].crb & 1) && !(cia[i].crb & 0x40))
    return (UBY)(ciaStabilizeValue(cia[i].tbleft)>>8);
  return (UBY)(cia[i].tb>>8);
}

void ciaWritetblo(ULO i, UBY data)
{
  cia[i].tblatch = (cia[i].tblatch & 0xff00) | ((ULO)data);
#ifdef CIA_LOGGING  
  Service->Log.AddLogDebug("CIA %c Timer B written (low-part): %X PC %X\n", (i == 0) ? 'A' : 'B', cia[i].tblatch, cpuGetOriginalPC());
#endif
}

void ciaWritetbhi(ULO i, UBY data)
{
  cia[i].tblatch = (cia[i].tblatch & 0xff) | (((ULO)data)<<8);

  if (ciaMustReloadOnTHiWrite(cia[i].crb)) // Reload when not started, or one-shot mode
  {
    cia[i].tb = ciaGetTimerValue(cia[i].tblatch);
    cia[i].tb_rem = 0;
    cia[i].tbleft = BUS_CYCLE_DISABLE;
  }

#ifdef CIA_LOGGING  
  Service->Log.AddLogDebug("CIA %c Timer B (hi-part) written: %X PC %X\n", (i == 0) ? 'A' : 'B', cia[i].tblatch, cpuGetOriginalPC());
#endif
  if (cia[i].crb & 8) // Timer B is one-shot, write starts it
  {
    cia[i].crb |= 1;
    ciaUnstabilize(i);
    ciaSetupNextEvent();
#ifdef CIA_LOGGING  
    Service->Log.AddLogDebug("CIA %c Timer B one-shot mode automatically started. PC %X\n", (i == 0) ? 'A' : 'B', cpuGetOriginalPC());
#endif
  }
}

/* Event counter */

UBY ciaReadevlo(ULO i)
{
  if (cia[i].evlatching)
  {
    cia[i].evlatching = FALSE;
    return (UBY)cia[i].evlatch;
  }
  return (UBY)cia[i].ev;
}

UBY ciaReadevmi(ULO i)
{
  if (cia[i].evlatching)
  {
    return (UBY)(cia[i].evlatch >> 8);
  }
  return (UBY)(cia[i].ev>>8);
}

UBY ciaReadevhi(ULO i)
{
  cia[i].evlatching = TRUE;
  cia[i].evlatch = cia[i].ev;
  return (UBY)(cia[i].ev>>16);
}

void ciaWriteevlo(ULO i, UBY data)
{
  if (cia[i].crb & 0x80)  // Alarm
  {
    cia[i].evalarm = (cia[i].evalarm & 0xffff00) | (ULO)data;
  }
  else // Time of day
  {
    cia[i].evwritelatching = FALSE;
    cia[i].evwritelatch = (cia[i].evwritelatch & 0xffff00) | (ULO)data;
    cia[i].ev = cia[i].evwritelatch;
  }
  ciaCheckAlarmMatch(i);
}

void ciaWriteevmi(ULO i, UBY data)
{
  if (cia[i].crb & 0x80)  // Alarm
  {
    cia[i].evalarm = (cia[i].evalarm & 0xff00ff) | ((ULO)data << 8);
    ciaCheckAlarmMatch(i);
  }
  else // Time of day
  {
    cia[i].evwritelatching = TRUE;
    cia[i].evwritelatch = (cia[i].evwritelatch & 0xff00ff) | (((ULO)data) << 8);
  }
}

void ciaWriteevhi(ULO i, UBY data)
{
  if (cia[i].crb & 0x80)  // Alarm
  {
    cia[i].evalarm = (cia[i].evalarm & 0xffff) | ((ULO)data << 16);
    ciaCheckAlarmMatch(i);
  }
  else // Time of day
  {
    cia[i].evwritelatching = TRUE;
    cia[i].evwritelatch = (cia[i].evwritelatch & 0xffff) | (((ULO)data) << 16);
  }
}

/* ICR */

UBY ciaReadicr(ULO i)
{
  UBY tmp = cia[i].icrreq;
  cia[i].icrreq = 0;

#ifdef CIA_LOGGING 
  Service->Log.AddLogDebug("CIA %c ICR read, req is %X\n", (i == 0) ? 'A' : 'B', cia[i].icrreq);
#endif

  return tmp;
}

void ciaWriteicr(ULO i, UBY data)
{
  ULO old = cia[i].icrmsk;
  if (data & 0x80)
  {
    cia[i].icrmsk |= (data & 0x1f);
  }
  else
  {
    cia[i].icrmsk &= ~(data & 0x1f);
  }

  ciaUpdateIRQ(i);

#ifdef CIA_LOGGING  
  Service->Log.AddLogDebug("CIA %c IRQ mask data %X, mask was %X is %X. PC %X\n", (i == 0) ? 'A' : 'B', data, old, cia[i].icrmsk, cpuGetOriginalPC());
#endif
}

/* CRA */

UBY ciaReadcra(ULO i)
{
  return cia[i].cra;
}

void ciaWritecra(ULO i, UBY data)
{
  ciaStabilize(i);
  if (data & 0x10) // Force load
  {
    cia[i].ta = ciaGetTimerValue(cia[i].talatch);
    cia[i].ta_rem = 0;
    data &= 0xef; // Clear force load bit
#ifdef CIA_LOGGING  
    Service->Log.AddLogDebug("CIA %c Timer A force load %X. PC %X\n", (i == 0) ? 'A' : 'B', cia[i].ta, cpuGetOriginalPC());
#endif
  }
#ifdef CIA_LOGGING  
  if ((data & 1) != (cia[i].cra & 1))
  {
    Service->Log.AddLogDebug("CIA %c Timer A is %s, was %s. PC %X\n", (i == 0) ? 'A' : 'B', (data & 1) ? "started" : "stopped", (cia[i].cra & 1) ? "started" : "stopped", cpuGetOriginalPC());
  }
#endif
  cia[i].cra = data;
  ciaUnstabilize(i);
  ciaSetupNextEvent();
}

/* CRB */

UBY ciaReadcrb(ULO i)
{
  return cia[i].crb;
}

void ciaWritecrb(ULO i, UBY data)
{
  ciaStabilize(i);
  if (data & 0x10) // Force load
  {
    cia[i].tb = ciaGetTimerValue(cia[i].tblatch);
    cia[i].tb_rem = 0;
    data &= 0xef; // Clear force load bit
#ifdef CIA_LOGGING  
    Service->Log.AddLogDebug("CIA %c Timer B force load %X. PC %X\n", (i == 0) ? 'A' : 'B', cia[i].tb, cpuGetOriginalPC());
#endif
  }
#ifdef CIA_LOGGING  
  if ((data & 1) != (cia[i].cra & 1))
  {
    Service->Log.AddLogDebug("CIA %c Timer B is %s, was %s. PC %X\n", (i == 0) ? 'A' : 'B', (data & 1) ? "started" : "stopped", (cia[i].crb & 1) ? "started" : "stopped", cpuGetOriginalPC());
  }
#endif
  cia[i].crb = data;
  ciaUnstabilize(i);
  ciaSetupNextEvent();
}  

/* Dummy read and write */

UBY ciaReadNothing(ULO i)
{
  return 0xff;
}

void ciaWriteNothing(ULO i, UBY data)
{
}

/* Table of CIA read/write functions */

ciaFetchFunc cia_read[16] =
{
  ciaReadpra, ciaReadprb, ciaReadddra,ciaReadddrb,
  ciaReadtalo,ciaReadtahi,ciaReadtblo,ciaReadtbhi,
  ciaReadevlo,ciaReadevmi,ciaReadevhi,ciaReadNothing,
  ciaReadsp,  ciaReadicr, ciaReadcra, ciaReadcrb
};
ciaWriteFunc cia_write[16] =
{
  ciaWritepra, ciaWriteprb, ciaWriteddra,ciaWriteddrb,
  ciaWritetalo,ciaWritetahi,ciaWritetblo,ciaWritetbhi,
  ciaWriteevlo,ciaWriteevmi,ciaWriteevhi,ciaWriteNothing,
  ciaWritesp,  ciaWriteicr, ciaWritecra, ciaWritecrb
};

  UBY ciaReadByte(ULO address)
  {
    if ((address & 0xa01001) == 0xa00001)
      return cia_read[(address & 0xf00)>>8](0);
    else if ((address & 0xa02001) == 0xa00000)
      return cia_read[(address & 0xf00)>>8](1);
    return 0xff;
  }

  UWO ciaReadWord(ULO address)
  {
    return (((UWO)ciaReadByte(address))<<8) | ((UWO)ciaReadByte(address + 1));
  }

  ULO ciaReadLong(ULO address)
  {
    return (((ULO)ciaReadByte(address))<<24)    | (((ULO)ciaReadByte(address + 1))<<16) |
      (((ULO)ciaReadByte(address + 2))<<8) | ((ULO)ciaReadByte(address + 3));
  }

  void ciaWriteByte(UBY data, ULO address)
  {
    if ((address & 0xa01001) == 0xa00001)
      cia_write[(address & 0xf00)>>8](0, data);
    else if ((address & 0xa02001) == 0xa00000)
      cia_write[(address & 0xf00)>>8](1, data);
  }

  void ciaWriteWord(UWO data, ULO address)
  {
    ciaWriteByte((UBY) (data>>8), address);
    ciaWriteByte((UBY) data, address + 1);
  }

  void ciaWriteLong(ULO data, ULO address)
  {
    ciaWriteByte((UBY) (data>>24), address);
    ciaWriteByte((UBY) (data>>16), address + 1);
    ciaWriteByte((UBY) (data>>8), address + 2);
    ciaWriteByte((UBY) data, address + 3);
  }

  /*============================================================================*/
  /* Map cia banks into the memory table                                        */
  /*============================================================================*/

  void ciaMemoryMap(void)
  {
    ULO bank;

    for (bank = 0xa00000>>16; bank < (0xc00000>>16); bank++)
      memoryBankSet(ciaReadByte,
      ciaReadWord,
      ciaReadLong,
      ciaWriteByte,
      ciaWriteWord,
      ciaWriteLong,
      NULL,
      bank,
      0xa00000>>16,
      FALSE);
  }

  /*============================================================================*/
  /* Cia state zeroing                                                          */
  /*============================================================================*/

  void ciaStateClear(void)
  {
    ULO i;

    for (i = 0; i < 2; i++)
    {
      cia[i].ev = 0;		/* Zero out event counters */
      cia[i].evlatch = 0;
      cia[i].evlatching = 0;
      cia[i].evalarm = 0;
      cia[i].evalarmlatch = 0;
      cia[i].evalarmlatching = 0;
      cia[i].evwritelatch = 0;
      cia[i].evwritelatching = 0;
      cia[i].taleft = BUS_CYCLE_DISABLE;		/* Zero out timers */
      cia[i].tbleft = BUS_CYCLE_DISABLE;
      cia[i].ta = 0xffff;        
      cia[i].tb = 0xffff;
      cia[i].talatch = 0xffff;
      cia[i].tblatch = 0xffff;
      cia[i].pra = 0xff;
      cia[i].prb = 0;
      cia[i].ddra = 0;
      cia[i].ddrb = 0;
      cia[i].icrreq = 0;
      cia[i].icrmsk = 0;
      cia[i].cra = 0;
      cia[i].crb = 0;
    }
    cia_recheck_irq = false;
    cia_recheck_irq_time = BUS_CYCLE_DISABLE;
    cia_next_event_type = 0;
  }

  /*============================================================================*/
  /* Cia module control                                                         */
  /*============================================================================*/

  void ciaSaveState(FILE *F)
  {
    ULO i;

    for (i = 0; i < 2; i++)
    {
      fwrite(&cia[i].ev, sizeof(cia[i].ev), 1, F);
      fwrite(&cia[i].evlatch, sizeof(cia[i].evlatch), 1, F);
      fwrite(&cia[i].evlatching, sizeof(cia[i].evlatching), 1, F);
      fwrite(&cia[i].evalarm, sizeof(cia[i].evalarm), 1, F);
      fwrite(&cia[i].evalarmlatch, sizeof(cia[i].evalarmlatch), 1, F);
      fwrite(&cia[i].evalarmlatching, sizeof(cia[i].evalarmlatching), 1, F);
      fwrite(&cia[i].evwritelatch, sizeof(cia[i].evwritelatch), 1, F);
      fwrite(&cia[i].evwritelatching, sizeof(cia[i].evwritelatching), 1, F);
      fwrite(&cia[i].taleft, sizeof(cia[i].taleft), 1, F);
      fwrite(&cia[i].tbleft, sizeof(cia[i].tbleft), 1, F);
      fwrite(&cia[i].ta, sizeof(cia[i].ta), 1, F);
      fwrite(&cia[i].tb, sizeof(cia[i].tb), 1, F);
      fwrite(&cia[i].talatch, sizeof(cia[i].talatch), 1, F);
      fwrite(&cia[i].tblatch, sizeof(cia[i].tblatch), 1, F);
      fwrite(&cia[i].pra, sizeof(cia[i].pra), 1, F);
      fwrite(&cia[i].prb, sizeof(cia[i].prb), 1, F);
      fwrite(&cia[i].ddra, sizeof(cia[i].ddra), 1, F);
      fwrite(&cia[i].ddrb, sizeof(cia[i].ddrb), 1, F);
      fwrite(&cia[i].icrreq, sizeof(cia[i].icrreq), 1, F);
      fwrite(&cia[i].icrmsk, sizeof(cia[i].icrmsk), 1, F);
      fwrite(&cia[i].cra, sizeof(cia[i].cra), 1, F);
      fwrite(&cia[i].crb, sizeof(cia[i].crb), 1, F);
    }
    fwrite(&cia_next_event_type, sizeof(cia_next_event_type), 1, F);
  }

  void ciaLoadState(FILE *F)
  {
    ULO i;

    for (i = 0; i < 2; i++)
    {
      fread(&cia[i].ev, sizeof(cia[i].ev), 1, F);
      fread(&cia[i].evlatch, sizeof(cia[i].evlatch), 1, F);
      fread(&cia[i].evlatching, sizeof(cia[i].evlatching), 1, F);
      fread(&cia[i].evalarm, sizeof(cia[i].evalarm), 1, F);
      fread(&cia[i].evalarmlatch, sizeof(cia[i].evalarmlatch), 1, F);
      fread(&cia[i].evalarmlatching, sizeof(cia[i].evalarmlatching), 1, F);
      fread(&cia[i].evwritelatch, sizeof(cia[i].evwritelatch), 1, F);
      fread(&cia[i].evwritelatching, sizeof(cia[i].evwritelatching), 1, F);
      fread(&cia[i].taleft, sizeof(cia[i].taleft), 1, F);
      fread(&cia[i].tbleft, sizeof(cia[i].tbleft), 1, F);
      fread(&cia[i].ta, sizeof(cia[i].ta), 1, F);
      fread(&cia[i].tb, sizeof(cia[i].tb), 1, F);
      fread(&cia[i].talatch, sizeof(cia[i].talatch), 1, F);
      fread(&cia[i].tblatch, sizeof(cia[i].tblatch), 1, F);
      fread(&cia[i].pra, sizeof(cia[i].pra), 1, F);
      fread(&cia[i].prb, sizeof(cia[i].prb), 1, F);
      fread(&cia[i].ddra, sizeof(cia[i].ddra), 1, F);
      fread(&cia[i].ddrb, sizeof(cia[i].ddrb), 1, F);
      fread(&cia[i].icrreq, sizeof(cia[i].icrreq), 1, F);
      fread(&cia[i].icrmsk, sizeof(cia[i].icrmsk), 1, F);
      fread(&cia[i].cra, sizeof(cia[i].cra), 1, F);
      fread(&cia[i].crb, sizeof(cia[i].crb), 1, F);
    }
    fread(&cia_next_event_type, sizeof(cia_next_event_type), 1, F);
  }

  void ciaEmulationStart(void) {
  }

  void ciaEmulationStop(void) {
  }

  void ciaHardReset(void) {
    ciaStateClear();
    ciaMemoryMap();
  }

  void ciaStartup(void) {
    ciaStateClear();
  }

  void ciaShutdown(void) {
  }
