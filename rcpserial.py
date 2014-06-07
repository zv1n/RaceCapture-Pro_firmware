import serial
import io
import json
import traceback
import Queue
from threading import Thread
from rcpconfig import *
from serial.tools import list_ports

CHANNEL_ADD_MODE_IN_PROGRESS = 1
CHANNEL_ADD_MODE_COMPLETE = 2
DEFAULT_READ_RETRIES = 5

    

class RcpCmd:
    cmd = None
    payload = None
    callback = None
    def __init__(self, **kwargs):
        self.cmd = kwargs.get('cmd', None)
        self.callback = kwargs.get('callback', None)
        self.payload = kwargs.get('payload', None)

            
class RcpSerial:    
    msgListeners = {}
    cmdQueue = Queue.Queue()
    cmdSequenceQueue = Queue.Queue()
    
    def __init__(self, **kwargs):
        self.ser = None
        self.port = kwargs.get('port', None)
     
    def setPort(self, port):
        self.port = port
        
    def initSerial(self):
        rxThread = Thread(target=self.msgRxWorker)
        rxThread.daemon = True
        rxThread.start()

        txThread = Thread(target=self.msgTxWorker)
        txThread.daemon = True
        txThread.start()
        

    def addListener(self, messageName, callback):
        listeners = self.msgListeners.get(messageName, None)
        if listeners:
            listeners.add(callback)
        else:
            listeners = set()
            listeners.add(callback)
            self.msgListeners[messageName] = listeners

    def msgRxWorker(self):
        print('msgRxWorker started')
        while True:
            serial = self.getSerial()
            try:
                msg = self.readLine(serial)
                print('msgRxWorker Rx: ' + str(msg))
                msgJson = json.loads(msg)
                for messageName in msgJson.keys():
                    print('processing messag ' + messageName)
                    listeners = self.msgListeners.get(messageName, None)
                    if listeners:
                        for listener in listeners:
                            print('listener ' + str(listener))
                            listener(msgJson)
                    break
            except Exception:
                print('Message Rx Exception: ' + str(Exception))
                traceback.print_exc()
        
    def msgTxWorker(self):
        q = self.cmdQueue
        while True:
            try:
                cmd = q.get()
                print('worker: ' + str(cmd))
                
                payload = cmd.payload
                if payload:
                    rsp = cmd.cmd(payload)
                else:
                    rsp = cmd.cmd()
                    
                callback = cmd.callback
                if callback:
                    callback(rsp) 
                q.task_done()
            except Exception:
                print('Aysnc command exception: ' + str(Exception))
                traceback.print_exc()

    def setListeners(self, messageNames, callback):
        for message in messageNames:
            self.addListener(message['name'], callback)
            
    def rcpCfgComplete(self, msgReply):
        self.cmdSequenceQueue.put(msgReply)
                
    def getRcpCfgSequence(self, rootName, callback):
        getRcpCfgSequence = [ {'name': 'ver',       'cmd': self.getVersion},
                              {'name': 'analogCfg', 'cmd': self.getAnalogCfg},
                              {'name': 'imuCfg',    'cmd': self.getImuCfg},
                              {'name': 'gpsCfg',    'cmd': self.getGpsCfg},
                              {'name': 'timerCfg',  'cmd': self.getTimerCfg},
                              {'name': 'gpioCfg',   'cmd': self.getGpioCfg},
                              {'name': 'pwmCfg',    'cmd': self.getPwmCfg},
                              {'name': 'trackCfg',  'cmd': self.getTrackCfg},
                              {'name': 'canCfg',    'cmd': self.getCanCfg},
                              {'name': 'obd2Cfg',   'cmd': self.getObd2Cfg},
                              {'name': 'scriptCfg', 'cmd': self.getScript},
                              {'name': 'connCfg',   'cmd': self.getConnectivityCfg}
                           ]
        
        self.setListeners(getRcpCfgSequence, self.rcpCfgComplete)
        
        q = self.cmdSequenceQueue
        
        rcpCfg = {}
        try:
            for rcpCfgCmd in getRcpCfgSequence:
                print('calling cfgCmd: ' + str(rcpCfgCmd))
                cmd = rcpCfgCmd['cmd']
                cmd()
                result = q.get()
                name = rcpCfgCmd['name']
                rcpCfg[name] = result[name]
            print('full cfg ' + str(rcpCfg))
            callback({rootName: rcpCfg})
        except Exception:
            print('getRcpCfg sequence exception: ' + str(Exception))
            traceback.print_exc()
        
    def queueCommand(self, cmd, callback, payload = None):
        self.cmdQueue.put(RcpCmd(cmd=cmd, callback=callback, payload=payload))
        
    def sendCommand(self, cmd, retry = DEFAULT_READ_RETRIES):
        rsp = None
        ser = self.getSerial()
        ser.flushInput()
        ser.flushOutput()
        cmdStr = json.dumps(cmd, separators=(',', ':')) + '\r'
        print('send cmd: ' + cmdStr)
        ser.write(cmdStr)
        
    def sendGet(self, name, index):
        if index == None:
            index = None
        else:
            index = str(index)
        cmd = {name:index}
        return self.sendCommand(cmd)

    def getSerial(self):
        if not self.ser:
            ser = self.open()
            ser.flushInput()
            ser.flushOutput()
            self.ser = ser
        return self.ser
            
    def open(self):
        print('Opening serial')
        ser = serial.Serial(self.port, timeout = None)
        ser.flushInput()
        ser.flushOutput()
        return ser

    def close(self):
        if self.ser != None:
            self.ser.close()
        self.ser = None
    
    def readLine(self, ser, retries = DEFAULT_READ_RETRIES):
        eol2 = b'\r'
        retryCount = 0
        line = bytearray()

        while True:
            c = ser.read(1)
            if  c == eol2:
                break
            elif c == '':
                if retryCount >= retries:
                    raise Exception('Could not read message')
                retryCount +=1
                print('Timeout - retry: ' + str(retryCount))
                ser.write(' ')
            else:
                line += c
                
        line = bytes(line).strip()
        line = line.replace('\r', '')
        line = line.replace('\n', '')
        return line
        
    def getRcpCfg(self, callback):
        getRcpCfgThread = Thread(target=self.getRcpCfgSequence, args=('rcpCfg', callback,))
        getRcpCfgThread.daemon = True
        getRcpCfgThread.start()
            
    def writeRcpCfg(self, cfg, callback = None):
        if callback:
            self.queueCommand(self.writeRcpCfg, callback, cfg)
        else:
            rcpCfg = cfg.get('rcpCfg', None)
            if rcpCfg:
                gpsCfg = rcpCfg.get('gpsCfg', None)
                if gpsCfg:
                    self.sendCommand({'setGpsCfg': gpsCfg})
                    
                imuCfg = rcpCfg.get('imuCfg', None)
                if imuCfg:
                    for i in range(IMU_CHANNEL_COUNT):
                        imuChannel = imuCfg.get(str(i))
                        if imuChannel:
                            self.sendCommand({'setImuCfg': {str(i): imuChannel}})
                
                analogCfg = rcpCfg.get('analogCfg', None)
                if analogCfg:
                    for i in range(ANALOG_CHANNEL_COUNT):
                        analogChannel = analogCfg.get(str(i))
                        if analogChannel:
                            self.sendCommand({'setAnalogCfg': {str(i): analogChannel}})
            
                timerCfg = rcpCfg.get('timerCfg', None)
                if timerCfg:
                    for i in range(TIMER_CHANNEL_COUNT):
                        timerChannel = timerCfg.get(str(i))
                        if timerChannel:
                            self.sendCommand({'setTimerCfg': {str(i): timerChannel}})
                            
                gpioCfg = rcpCfg.get('gpioCfg', None)
                if gpioCfg:
                    for i in range(GPIO_CHANNEL_COUNT):
                        gpioChannel = gpioCfg.get(str(i))
                        if gpioChannel:
                            self.sendCommand({'setGpioCfg': {str(i): gpioChannel}})
    
                pwmCfg = rcpCfg.get('pwmCfg', None)
                if pwmCfg:
                    for i in range(PWM_CHANNEL_COUNT):
                        pwmChannel = pwmCfg.get(str(i))
                        if pwmChannel:
                            self.sendCommand({'setPwmCfg': {str(i): pwmChannel}})
    
                connCfg = rcpCfg.get('connCfg', None)
                if connCfg:
                    self.sendCommand({'setConnCfg' : connCfg})
                    
                canCfg = rcpCfg.get('canCfg', None)
                if canCfg:
                    self.sendCommand({'setCanCfg': canCfg})
                    
                obd2Cfg = rcpCfg.get('obd2Cfg', None)
                if obd2Cfg:
                    self.sendCommand({'setObd2Cfg': obd2Cfg})
                    
                trackCfg = rcpCfg.get('trackCfg', None)
                if trackCfg:
                    self.sendCommand({'setTrackCfg': trackCfg})
                    
                scriptCfg = rcpCfg.get('scriptCfg', None)
                if scriptCfg:
                    self.writeScript(scriptCfg)
                                        
                self.flashConfig()
                
                return True
        
                
    def getAnalogCfg(self, channelId = None):
        return self.sendGet('getAnalogCfg', channelId)    

    def getImuCfg(self, channelId = None):
        return self.sendGet('getImuCfg', channelId)
    
    def getGpsCfg(self):
        return self.sendGet('getGpsCfg', None)
    
    def setGpsCfg(self, gpsCfg):
        return self.sendSet('setGpsCfg', gpsCfg)
    
    def getTimerCfg(self, channelId = None):
        return self.sendGet('getTimerCfg', channelId)
    
    def getGpioCfg(self, channelId = None):
        return self.sendGet('getGpioCfg', channelId)
    
    def getPwmCfg(self, channelId = None):
        return self.sendGet('getPwmCfg', channelId)
    
    def getTrackCfg(self):
        return self.sendGet('getTrackCfg', None)
    
    def getCanCfg(self):
        return self.sendGet('getCanCfg', None)
    
    def getObd2Cfg(self):
        return self.sendGet('getObd2Cfg', None)
    
    def getConnectivityCfg(self):
        return self.sendGet('getConnCfg', None)
    
    def getScript(self):
        return self.sendGet('getScriptCfg', None)

    def writeScript(self, scriptCfg):
        i = 0
        res = 0
        script = scriptCfg['data']
        while True:
            if len(script) >= 256:
                scr = script[:256]
                script = script[256:]
                res = self.sendCommand({'setScriptCfg': {'data':scr,'page':i}})
                if res == 0:
                    print 'Error: ' + str(i)
                    break
                i = i + 1
            else:
                self.sendCommand({'setScriptCfg': {'data':script,'page':i}})
                self.sendCommand({'setScriptCfg': {'data':'','page':i + 1}})
                res = 1
                break
        return res
        
    def flashConfig(self):
        return self.sendCommand({'flashCfg':None})

    def getChannels(self, callback):
        if callback:
            self.queueCommand(self.getChannels, callback)
        else:
            return self.sendGet("getChannels", None)
                
    def addChannel(self, channelJson, index, mode):
        return self.sendCommand({'addChannel': 
                                 {'index': index, 
                                 'mode': mode,
                                 'channel': channelJson
                                 }
                                 })
                                  
    def getVersion(self):
        rsp = self.sendCommand({"getVer":None}, 1)
        return rsp

    def autoDetect(self):
        ports = [x[0] for x in list_ports.comports()]

        print "Searching for RaceCapture on all serial ports"
        testVer = VersionConfig()
        verJson = None
        for p in ports:
            try:
                print "Trying", p
                self.port = p
                verJson = self.getVersion()
                testVer.fromJson(verJson.get('ver', None))
                if testVer.major > 0 or testVer.minor > 0 or testVer.bugfix > 0:
                    break
            except Exception as detail:
                print('Failed: ' + str(detail))
                self.port = None
                self.close()

        if not verJson == None:
            print "Found racecapture version " + testVer.toString() + " on port:", self.port


    
