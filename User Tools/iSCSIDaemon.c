/*!
 * @author		Nareg Sinenian
 * @file		iSCSIDaemon.c
 * @version		1.0
 * @copyright	(c) 2014-2015 Nareg Sinenian. All rights reserved.
 * @brief		iSCSI user-space daemon
 */


// BSD includes
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

// Foundation includes
#include <launch.h>
#include <CoreFoundation/CFPreferences.h>

// Mach kernel includes
#include <mach/mach_port.h>
#include <mach/mach_init.h>
#include <mach/mach_interface.h>

// I/O Kit includes
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/IOMessage.h>

// iSCSI includes
#include "iSCSISession.h"
#include "iSCSIDaemonInterfaceShared.h"
#include "iSCSIPropertyList.h"

// Used to notify daemon of power state changes
io_connect_t powerPlaneRoot;
io_object_t powerNotifier;
IONotificationPortRef powerNotifyPortRef;

const struct iSCSIDRspLogin iSCSIDRspLoginInit = {
    .funcCode = kiSCSIDLogin,
    .errorCode = 0,
    .statusCode = (UInt8)kiSCSILoginInvalidStatusCode
};

const struct iSCSIDRspLogout iSCSIDRspLogoutInit = {
    .funcCode = kiSCSIDLogout,
    .errorCode = 0,
    .statusCode = (UInt8)kiSCSILogoutInvalidStatusCode
};

const struct iSCSIDRspCreateArrayOfActiveTargets iSCSIDRspCreateArrayOfActiveTargetsInit = {
    .funcCode = kiSCSIDCreateArrayOfActiveTargets,
    .errorCode = 0,
    .dataLength = 0
};

const struct iSCSIDRspCreateArrayOfActivePortalsForTarget iSCSIDRspCreateArrayOfActivePortalsForTargetInit = {
    .funcCode = kiSCSIDCreateArrayOfActivePortalsForTarget,
    .errorCode = 0,
    .dataLength = 0
};

const struct iSCSIDRspIsTargetActive iSCSIDRspIsTargetActiveInit = {
    .funcCode = kiSCSIDIsTargetActive,
    .active = false
};

const struct iSCSIDRspIsPortalActive iSCSIDRspIsPortalActiveInit = {
    .funcCode = kiSCSIDIsPortalActive,
    .active = false
};

const struct iSCSIDRspQueryPortalForTargets iSCSIDRspQueryPortalForTargetsInit = {
    .funcCode = kiSCSIDQueryPortalForTargets,
    .errorCode = 0,
    .statusCode = (UInt8) kiSCSILogoutInvalidStatusCode,
    .discoveryLength = 0
};

const struct iSCSIDRspQueryTargetForAuthMethod iSCSIDRspQueryTargetForAuthMethodInit = {
    .funcCode = kiSCSIDQueryTargetForAuthMethod,
    .errorCode = 0,
    .statusCode = 0,
    .authMethod = 0
};

const struct iSCSIDRspCreateCFPropertiesForSession iSCSIDRspCreateCFPropertiesForSessionInit = {
    .funcCode = kiSCSIDCreateCFPropertiesForSession,
    .errorCode = 0,
    .dataLength = 0
};

const struct iSCSIDRspCreateCFPropertiesForConnection iSCSIDRspCreateCFPropertiesForConnectionInit = {
    .funcCode = kiSCSIDCreateCFPropertiesForConnection,
    .errorCode = 0,
    .dataLength = 0
};



errno_t iSCSIDLoginCommon(SID sessionId,
                          iSCSITargetRef target,
                          iSCSIPortalRef portal,
                          enum iSCSILoginStatusCode * statusCode)
{
    errno_t error = 0;
    iSCSISessionConfigRef sessCfg = NULL;
    iSCSIConnectionConfigRef connCfg = NULL;
    iSCSIAuthRef auth = NULL;

    CID connectionId = kiSCSIInvalidConnectionId;

    *statusCode = kiSCSILoginInvalidStatusCode;

    CFStringRef targetIQN = iSCSITargetGetIQN(target);

    // If session needs to be logged in, copy session config from property list
    if(sessionId == kiSCSIInvalidSessionId)
        if(!(sessCfg = iSCSIPLCopySessionConfig(targetIQN)))
            sessCfg = iSCSISessionConfigCreateMutable();

    // Get connection configuration from property list, create one if needed
    if(!(connCfg = iSCSIPLCopyConnectionConfig(targetIQN,iSCSIPortalGetAddress(portal))))
        connCfg = iSCSIConnectionConfigCreateMutable();

    // Get authentication configuration from property list, create one if needed
    if(!(auth = iSCSIPLCopyAuthenticationForTarget(targetIQN)))
        auth = iSCSIAuthCreateNone();

    // Do either session or connection login
    if(sessionId == kiSCSIInvalidSessionId)
        error = iSCSILoginSession(target,portal,auth,sessCfg,connCfg,&sessionId,&connectionId,statusCode);
    else
        error = iSCSILoginConnection(sessionId,portal,auth,connCfg,&connectionId,statusCode);
    
    return error;
}


errno_t iSCSIDLoginAllPortals(iSCSITargetRef target,
                              enum iSCSILoginStatusCode * statusCode)
{
    CFIndex activeConnections = 0;
    CFIndex maxConnections = 0;

    // Error code to return to daemon's client
    errno_t errorCode = 0;
    *statusCode = kiSCSILoginInvalidStatusCode;

    CFStringRef targetIQN = iSCSITargetGetIQN(target);
    SID sessionId = iSCSIGetSessionIdForTarget(targetIQN);

    // Set initial values for maxConnections and activeConnections
    if(sessionId == kiSCSIInvalidSessionId)
        maxConnections = 1;
    else {

        // If session exists, get the max connections and active connections
        CFDictionaryRef properties = iSCSICreateCFPropertiesForSession(target);

        if(properties) {
            // Get max connections from property list
            CFNumberRef number = CFDictionaryGetValue(properties,kRFC3720_Key_MaxConnections);
            CFNumberGetValue(number,kCFNumberSInt32Type,&maxConnections);
            CFRelease(properties);

            CFArrayRef connections = iSCSICreateArrayOfConnectionsIds(sessionId);

            if(connections) {
                activeConnections = CFArrayGetCount(connections);
                CFRelease(connections);
            }
        }
    }

    // Add portals to the session until we've run out of portals to add or
    // reached the maximum connection limit
    CFStringRef portalAddress = NULL;
    CFArrayRef portals = iSCSIPLCreateArrayOfPortals(targetIQN);
    CFIndex portalIdx = 0;

    while( (activeConnections < maxConnections) &&
          (portalAddress = CFArrayGetValueAtIndex(portals,portalIdx)) )
    {
        // Get portal object and login
        iSCSIPortalRef portal = iSCSIPLCopyPortalForTarget(targetIQN,portalAddress);
        errorCode = iSCSIDLoginCommon(sessionId,target,portal,statusCode);
        iSCSIPortalRelease(portal);

        // Quit if there was an error communicating with the daemon
        if(errorCode)
            break;

        activeConnections++;
        portalIdx++;

        // Determine how many connections this session supports
        sessionId = iSCSIGetSessionIdForTarget(iSCSITargetGetIQN(target));

        // If this was the first connection of the session, get the number of
        // allowed maximum connections
        if(activeConnections == 1) {
            CFDictionaryRef properties = iSCSICreateCFPropertiesForSession(target);
            if(properties) {
                // Get max connections from property list
                CFNumberRef number = CFDictionaryGetValue(properties,kRFC3720_Key_MaxConnections);
                CFNumberGetValue(number,kCFNumberSInt32Type,&maxConnections);
                CFRelease(properties);
            }
        }
        
    };
    
    return errorCode;
}


errno_t iSCSIDLoginWithPortal(iSCSITargetRef target,
                              iSCSIPortalRef portal,
                              enum iSCSILoginStatusCode * statusCode)
{
    // Check for active sessions before attempting loginb
    SID sessionId = kiSCSIInvalidSessionId;
    CID connectionId = kiSCSIInvalidConnectionId;
    *statusCode = kiSCSILoginInvalidStatusCode;
    errno_t errorCode = 0;

    CFStringRef targetIQN = iSCSITargetGetIQN(target);
    sessionId = iSCSIGetSessionIdForTarget(targetIQN);

    // Existing session, add a connection
    if(sessionId != kiSCSIInvalidSessionId) {

        connectionId = iSCSIGetConnectionIdForPortal(sessionId,portal);

        // If there's an active session display error otherwise login
        if(connectionId != kiSCSIInvalidConnectionId)
        {} //iSCSICtlDisplayError("The specified target has an active session over the specified portal.");
        else {
            // See if the session can support an additional connection
            CFDictionaryRef properties = iSCSICreateCFPropertiesForSession(target);
            if(properties) {
                // Get max connections from property list
                UInt32 maxConnections;
                CFNumberRef number = CFDictionaryGetValue(properties,kRFC3720_Key_MaxConnections);
                CFNumberGetValue(number,kCFNumberSInt32Type,&maxConnections);
                CFRelease(properties);

                CFArrayRef connections = iSCSICreateArrayOfConnectionsIds(sessionId);
                if(connections)
                {
                    CFIndex activeConnections = CFArrayGetCount(connections);
                    if(activeConnections == maxConnections)
                    {} //iSCSICtlDisplayError("The active session cannot support additional connections.");
                    else
                        errorCode = iSCSIDLoginCommon(sessionId,target,portal,statusCode);
                    CFRelease(connections);
                }
            }
        }

    }
    else  // Leading login
        errorCode = iSCSIDLoginCommon(sessionId,target,portal,statusCode);

    return errorCode;
}


errno_t iSCSIDLogin(int fd,struct iSCSIDCmdLogin * cmd)
{

    // Grab objects from stream
    iSCSITargetRef target = iSCSIDCreateObjectFromSocket(fd,cmd->targetLength,
                                                         (void *(* )(CFDataRef))&iSCSITargetCreateWithData);

    iSCSIPortalRef portal = iSCSIDCreateObjectFromSocket(fd,cmd->portalLength,
                                                         (void *(* )(CFDataRef))&iSCSIPortalCreateWithData);

    // If portal and target are valid, login with portal.  Otherwise login to
    // target using all defined portals.
    errno_t errorCode = 0;
    enum iSCSILoginStatusCode statusCode;

    // Synchronize property list
    iSCSIPLSynchronize();

    if(target && portal)
        errorCode = iSCSIDLoginWithPortal(target,portal,&statusCode);
    else if(target)
        errorCode = iSCSIDLoginAllPortals(target,&statusCode);
    else
        return EINVAL;

    // Compose a response to send back to the client
    struct iSCSIDRspLogin rsp = iSCSIDRspLoginInit;
    rsp.errorCode = errorCode;
    rsp.statusCode = statusCode;

    if(send(fd,&rsp,sizeof(rsp),0) != sizeof(rsp))
        return EAGAIN;

    return 0;
}


errno_t iSCSIDLogout(int fd,struct iSCSIDCmdLogout * cmd)
{
    // Grab objects from stream
    iSCSITargetRef target = iSCSIDCreateObjectFromSocket(fd,cmd->targetLength,
                                                         (void *(* )(CFDataRef))&iSCSITargetCreateWithData);

    iSCSIPortalRef portal = iSCSIDCreateObjectFromSocket(fd,cmd->portalLength,
                                                         (void *(* )(CFDataRef))&iSCSIPortalCreateWithData);

    SID sessionId = kiSCSIInvalidSessionId;
    CID connectionId = kiSCSIInvalidConnectionId;
    enum iSCSILogoutStatusCode statusCode = kiSCSILogoutInvalidStatusCode;

    // Error code to return to daemon's client
    errno_t errorCode = 0;

    // Synchronize property list
    iSCSIPLSynchronize();

    // See if there exists an active session for this target
    if((sessionId = iSCSIGetSessionIdForTarget(iSCSITargetGetIQN(target))) == kiSCSIInvalidSessionId)
    {
        //iSCSICtlDisplayError("The specified target has no active session.");
        errorCode = EINVAL;
    }

    // See if there exists an active connection for this portal
    if(!errorCode && portal)
        connectionId = iSCSIGetConnectionIdForPortal(sessionId,portal);

    // If the portal was specified and a connection doesn't exist for it...
    if(!errorCode && portal && connectionId == kiSCSIInvalidConnectionId)
    {
        //iSCSICtlDisplayError("The specified portal has no active connections.");
        errorCode = EINVAL;
    }

    // At this point either the we logout the session or just the connection
    // associated with the specified portal, if one was specified
    if(!errorCode)
    {
        if(!portal)
            errorCode = iSCSILogoutSession(sessionId,&statusCode);
        else
            errorCode = iSCSILogoutConnection(sessionId,connectionId,&statusCode);
        /*
         if(!error)
         iSCSICtlDisplayLogoutStatus(statusCode,target,portal);
         else
         iSCSICtlDisplayError(strerror(error));*/
    }

    if(portal)
        iSCSIPortalRelease(portal);
    iSCSITargetRelease(target);

    // Compose a response to send back to the client
    struct iSCSIDRspLogout rsp = iSCSIDRspLogoutInit;
    rsp.errorCode = errorCode;
    rsp.statusCode = statusCode;

    if(send(fd,&rsp,sizeof(rsp),0) != sizeof(rsp))
        return EAGAIN;

    return 0;
}

errno_t iSCSIDCreateArrayOfActiveTargets(int fd,struct iSCSIDCmdCreateArrayOfActiveTargets * cmd)
{
    CFArrayRef sessionIds = iSCSICreateArrayOfSessionIds();
    CFIndex sessionCount = CFArrayGetCount(sessionIds);

    // Prepare an array to hold our targets
    CFMutableArrayRef activeTargets = CFArrayCreateMutable(kCFAllocatorDefault,
                                                           sessionCount,
                                                           &kCFTypeArrayCallBacks);

    // Get target object for each active session and add to array
    for(CFIndex idx = 0; idx < sessionCount; idx++)
    {
        iSCSITargetRef target = iSCSICreateTargetForSessionId((SID)CFArrayGetValueAtIndex(sessionIds,idx));
        CFArrayAppendValue(activeTargets,target);
        iSCSITargetRelease(target);
    }

    // Serialize and send array
    CFDataRef data = CFPropertyListCreateData(kCFAllocatorDefault,
                                              (CFPropertyListRef) activeTargets,
                                              kCFPropertyListBinaryFormat_v1_0,0,NULL);
    CFRelease(activeTargets);

    // Send response header
    struct iSCSIDRspCreateArrayOfActiveTargets rsp = iSCSIDRspCreateArrayOfActiveTargetsInit;
    if(data)
        rsp.dataLength = (UInt32)CFDataGetLength(data);
    else
        rsp.dataLength = 0;

    if(send(fd,&rsp,sizeof(rsp),0) != sizeof(rsp))
    {
        if(data)
            CFRelease(data);
        return EAGAIN;
    }

    if(data)
    {
        if(send(fd,CFDataGetBytePtr(data),rsp.dataLength,0) != rsp.dataLength) {
            CFRelease(data);
            return EAGAIN;
        }
        CFRelease(data);
    }
    return 0;
}

errno_t iSCSIDCreateArrayofActivePortalsForTarget(int fd,struct iSCSIDCmdCreateArrayOfActivePortalsForTarget * cmd)
{
    CFArrayRef sessionIds = iSCSICreateArrayOfSessionIds();
    CFIndex sessionCount = CFArrayGetCount(sessionIds);

    // Prepare an array to hold our targets
    CFMutableArrayRef activeTargets = CFArrayCreateMutable(kCFAllocatorDefault,
                                                           sessionCount,
                                                           &kCFTypeArrayCallBacks);

    // Get target object for each active session and add to array
    for(CFIndex idx = 0; idx < sessionCount; idx++)
    {
        iSCSITargetRef target = iSCSICreateTargetForSessionId((SID)CFArrayGetValueAtIndex(sessionIds,idx));
        CFArrayAppendValue(activeTargets,target);
        iSCSITargetRelease(target);
    }

    // Serialize and send array
    CFDataRef data = CFPropertyListCreateData(kCFAllocatorDefault,
                                              (CFPropertyListRef) activeTargets,
                                              kCFPropertyListBinaryFormat_v1_0,0,NULL);
    CFRelease(activeTargets);

    // Send response header
    struct iSCSIDRspCreateArrayOfActiveTargets rsp = iSCSIDRspCreateArrayOfActiveTargetsInit;
    if(data)
        rsp.dataLength = (UInt32)CFDataGetLength(data);
    else
        rsp.dataLength = 0;

    if(send(fd,&rsp,sizeof(rsp),0) != sizeof(rsp))
    {
        if(data)
            CFRelease(data);
        return EAGAIN;
    }

    if(data)
    {
        if(send(fd,CFDataGetBytePtr(data),rsp.dataLength,0) != rsp.dataLength)
        {
            CFRelease(data);
            return EAGAIN;
        }

        CFRelease(data);
    }
    return 0;
}

errno_t iSCSIDIsTargetActive(int fd,struct iSCSIDCmdIsTargetActive *cmd)
{
    // Grab objects from stream
    iSCSITargetRef target = iSCSIDCreateObjectFromSocket(fd,cmd->targetLength,
                                                         (void *(* )(CFDataRef))&iSCSITargetCreateWithData);

    iSCSIDRspIsTargetActive rsp = iSCSIDRspIsTargetActiveInit;
    rsp.active = (iSCSIGetSessionIdForTarget(iSCSITargetGetIQN(target)) != kiSCSIInvalidSessionId);

    if(send(fd,&rsp,sizeof(rsp),0) != sizeof(rsp))
        return EAGAIN;

    return 0;
}

errno_t iSCSIDIsPortalActive(int fd,struct iSCSIDCmdIsPortalActive *cmd)
{
    // Grab objects from stream
    iSCSITargetRef target = iSCSIDCreateObjectFromSocket(fd,cmd->targetLength,
                                                         (void *(* )(CFDataRef))&iSCSITargetCreateWithData);

    iSCSIPortalRef portal = iSCSIDCreateObjectFromSocket(fd,cmd->portalLength,
                                                         (void *(* )(CFDataRef))&iSCSIPortalCreateWithData);

    iSCSIDRspIsPortalActive rsp = iSCSIDRspIsPortalActiveInit;
    SID sessionId = (iSCSIGetSessionIdForTarget(iSCSITargetGetIQN(target)));

    if(sessionId == kiSCSIInvalidSessionId)
        rsp.active = false;
    else
        rsp.active = (iSCSIGetConnectionIdForPortal(sessionId,portal) != kiSCSIInvalidConnectionId);

    if(send(fd,&rsp,sizeof(rsp),0) != sizeof(rsp))
        return EAGAIN;

    return 0;
}


errno_t iSCSIDQueryPortalForTargets(int fd,struct iSCSIDCmdQueryPortalForTargets * cmd)
{
    // Grab objects from stream
    iSCSIPortalRef portal = iSCSIDCreateObjectFromSocket(fd,cmd->portalLength,
                                                         (void *(* )(CFDataRef))&iSCSIPortalCreateWithData);

    // Grab objects from stream
    iSCSIAuthRef auth = iSCSIDCreateObjectFromSocket(fd,cmd->authLength,
                                                     (void *(* )(CFDataRef))&iSCSIAuthCreateWithData);

    enum iSCSILoginStatusCode statusCode = kiSCSILoginInvalidStatusCode;

    iSCSIMutableDiscoveryRecRef discoveryRec;
    errno_t error = iSCSIQueryPortalForTargets(portal,auth,&discoveryRec,&statusCode);

    iSCSIPortalRelease(portal);
    iSCSIAuthRelease(auth);

    // Compose a response to send back to the client
    struct iSCSIDRspQueryPortalForTargets rsp = iSCSIDRspQueryPortalForTargetsInit;
    rsp.errorCode = error;
    rsp.statusCode = statusCode;
    CFDataRef data = NULL;

    // If a discovery record was returned, get data and free discovery object
    if(discoveryRec) {
        data = iSCSIDiscoveryRecCreateData(discoveryRec);
        iSCSIDiscoveryRecRelease(discoveryRec);
        rsp.discoveryLength = (UInt32)CFDataGetLength(data);
    }

    if(send(fd,&rsp,sizeof(rsp),0) != sizeof(rsp))
    {
        CFRelease(data);
        return EAGAIN;
    }

    // Send discovery data if any
    if(data) {
        if(send(fd,CFDataGetBytePtr(data),CFDataGetLength(data),0) != CFDataGetLength(data))
        {
            CFRelease(data);
            return EAGAIN;
        }
        CFRelease(data);
    }
    return 0;
}

errno_t iSCSIDQueryTargetForAuthMethod(int fd,struct iSCSIDCmdQueryTargetForAuthMethod * cmd)
{
    // Grab objects from stream
    iSCSITargetRef target = iSCSIDCreateObjectFromSocket(fd,cmd->targetLength,
                                                         (void *(* )(CFDataRef))&iSCSITargetCreateWithData);
    iSCSIPortalRef portal = iSCSIDCreateObjectFromSocket(fd,cmd->portalLength,
                                                         (void *(* )(CFDataRef))&iSCSIPortalCreateWithData);

    enum iSCSIAuthMethods authMethod = kiSCSIAuthMethodInvalid;
    enum iSCSILoginStatusCode statusCode = kiSCSILoginInvalidStatusCode;

    errno_t error = iSCSIQueryTargetForAuthMethod(portal,iSCSITargetGetIQN(target),&authMethod,&statusCode);

    // Compose a response to send back to the client
    struct iSCSIDRspQueryTargetForAuthMethod rsp = iSCSIDRspQueryTargetForAuthMethodInit;
    rsp.errorCode = error;
    rsp.statusCode = statusCode;
    rsp.authMethod = authMethod;

    if(send(fd,&rsp,sizeof(rsp),0) != sizeof(rsp))
        return EAGAIN;

    return 0;
}

errno_t iSCSIDCreateCFPropertiesForSession(int fd,struct iSCSIDCmdCreateCFPropertiesForSession * cmd)
{
    // Grab objects from stream
    iSCSITargetRef target = iSCSIDCreateObjectFromSocket(fd,cmd->targetLength,
                                                         (void *(* )(CFDataRef))&iSCSITargetCreateWithData);

    if(!target)
        return EINVAL;

    errno_t error = 0;
    CFDictionaryRef properties = iSCSICreateCFPropertiesForSession(target);

    // Send back response
    iSCSIDRspCreateCFPropertiesForSession rsp = iSCSIDRspCreateCFPropertiesForSessionInit;

    CFDataRef data = NULL;
    if(properties) {
        data = CFPropertyListCreateData(kCFAllocatorDefault,
                                        (CFPropertyListRef)properties,
                                        kCFPropertyListBinaryFormat_v1_0,0,NULL);

        rsp.dataLength = (UInt32)CFDataGetLength(data);
        CFRelease(properties);
    }
    else
        rsp.dataLength = 0;

    if(send(fd,&rsp,sizeof(rsp),0) != sizeof(rsp))
        error = EAGAIN;

    // Send data if any
    if(data && !error) {
        if(send(fd,CFDataGetBytePtr(data),CFDataGetLength(data),0) != CFDataGetLength(data))
            error =  EAGAIN;
        
        CFRelease(data);
    }
    return error;
}
errno_t iSCSIDCreateCFPropertiesForConnection(int fd,struct iSCSIDCmdCreateCFPropertiesForConnection * cmd)
{
    // Grab objects from stream
    iSCSITargetRef target = iSCSIDCreateObjectFromSocket(fd,cmd->targetLength,
                                                         (void *(* )(CFDataRef))&iSCSITargetCreateWithData);
    iSCSIPortalRef portal = iSCSIDCreateObjectFromSocket(fd,cmd->portalLength,
                                                         (void *(* )(CFDataRef))&iSCSIPortalCreateWithData);

    if(!target || !portal)
        return EINVAL;

    errno_t error = 0;
    CFDictionaryRef properties = iSCSICreateCFPropertiesForConnection(target,portal);

    // Send back response
    iSCSIDRspCreateCFPropertiesForConnection rsp = iSCSIDRspCreateCFPropertiesForConnectionInit;

    CFDataRef data = NULL;
    if(properties) {
         data = CFPropertyListCreateData(kCFAllocatorDefault,
                                         (CFPropertyListRef)properties,
                                         kCFPropertyListBinaryFormat_v1_0,0,NULL);

        rsp.dataLength = (UInt32)CFDataGetLength(data);
        CFRelease(properties);
    }
    else
        rsp.dataLength = 0;

    if(send(fd,&rsp,sizeof(rsp),0) != sizeof(rsp))
        error = EAGAIN;

    // Send data if any
    if(data && !error) {
        if(send(fd,CFDataGetBytePtr(data),CFDataGetLength(data),0) != CFDataGetLength(data))
                error =  EAGAIN;

        CFRelease(data);
    }

    return error;
}


/*! Handles power event messages received from the kernel.  This callback
 *  is only active when iSCSIDRegisterForPowerEvents() has been called.
 *  @param refCon always NULL (not used).
 *  @param service the service associated with the notification port.
 *  @param messageType the type of notification message.
 *  @param messageArgument argument associated with the notification message. */
void iSCSIDHandlePowerEvent(void * refCon,
                            io_service_t service,
                            natural_t messageType,
                            void * messageArgument)
{
    switch(messageType)
    {
            // TODO: handle sleep
            break;
    };

}

/*! Registers the daemon with the kernel to receive power events
 *  (e.g., sleep/wake notifications).
 *  @return true if the daemon was successfully registered. */
bool iSCSIDRegisterForPowerEvents()
{
    powerPlaneRoot = IORegisterForSystemPower(NULL,
                                              &powerNotifyPortRef,
                                              iSCSIDHandlePowerEvent,
                                              &powerNotifier);

    if(powerPlaneRoot == 0)
        return false;

    CFRunLoopAddSource(CFRunLoopGetMain(),
                       IONotificationPortGetRunLoopSource(powerNotifyPortRef),
                       kCFRunLoopDefaultMode);
    return true;
}

/*! Deregisters the daemon with the kernel to no longer receive power events. */
void iSCSIDDeregisterForPowerEvents()
{
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(),
                          IONotificationPortGetRunLoopSource(powerNotifyPortRef),
                          kCFRunLoopDefaultMode);

    IODeregisterForSystemPower(&powerNotifier);
    IOServiceClose(powerPlaneRoot);
    IONotificationPortDestroy(powerNotifyPortRef);
}

void iSCSIDProcessIncomingRequest(CFSocketRef socket,
                                  CFSocketCallBackType callbackType,
                                  CFDataRef address,
                                  const void * data,
                                  void * info)
{
    // File descriptor associated with the socket we're using
    static int fd = 0;

    // If this is the first connection, initialize the user client for the
    // iSCSI initiator kernel extension
    if(fd == 0) {
        iSCSIInitialize(CFRunLoopGetCurrent());

        // Wait for an incoming connection; upon timeout quit
        struct sockaddr_storage peerAddress;
        socklen_t length = sizeof(peerAddress);

        // Get file descriptor associated with the connection
        fd = accept(CFSocketGetNative(socket),(struct sockaddr *)&peerAddress,&length);
    }

    struct iSCSIDCmd cmd;

    while(recv(fd,&cmd,sizeof(cmd),MSG_PEEK) == sizeof(cmd)) {

        recv(fd,&cmd,sizeof(cmd),MSG_WAITALL);

        errno_t error = 0;
        switch(cmd.funcCode)
        {
            case kiSCSIDLogin:
                error = iSCSIDLogin(fd,(iSCSIDCmdLogin*)&cmd); break;
            case kiSCSIDLogout:
                error = iSCSIDLogout(fd,(iSCSIDCmdLogout*)&cmd); break;
            case kiSCSIDCreateArrayOfActiveTargets:
                error = iSCSIDCreateArrayOfActiveTargets(fd,(iSCSIDCmdCreateArrayOfActiveTargets*)&cmd); break;
            case kiSCSIDCreateArrayOfActivePortalsForTarget:
                error = iSCSIDCreateArrayofActivePortalsForTarget(fd,(iSCSIDCmdCreateArrayOfActivePortalsForTarget*)&cmd); break;
            case kiSCSIDIsTargetActive:
                error = iSCSIDIsTargetActive(fd,(iSCSIDCmdIsTargetActive*)&cmd); break;
            case kiSCSIDIsPortalActive:
                error = iSCSIDIsPortalActive(fd,(iSCSIDCmdIsPortalActive*)&cmd); break;
            case kiSCSIDQueryPortalForTargets:
                error = iSCSIDQueryPortalForTargets(fd,(iSCSIDCmdQueryPortalForTargets*)&cmd); break;
            case kiSCSIDQueryTargetForAuthMethod:
                error = iSCSIDQueryTargetForAuthMethod(fd,(iSCSIDCmdQueryTargetForAuthMethod*)&cmd); break;
            case kiSCSIDCreateCFPropertiesForSession:
                error = iSCSIDCreateCFPropertiesForSession(fd,(iSCSIDCmdCreateCFPropertiesForSession*)&cmd); break;
            case kiSCSIDCreateCFPropertiesForConnection:
                error = iSCSIDCreateCFPropertiesForConnection(fd,(iSCSIDCmdCreateCFPropertiesForConnection*)&cmd); break;
            default:
                // Close our connection to the iSCSI kernel extension
                iSCSICleanup();
                close(fd);
                fd = 0;
        };
    }
}

/*! iSCSI daemon entry point. */
int main(void)
{
    // Connect to the preferences .plist file associated with "iscsid" and
    // read configuration parameters for the initiator
    iSCSIPLSynchronize();

    CFStringRef initiatorIQN = iSCSIPLCopyInitiatorIQN();

    if(initiatorIQN) {
        iSCSISetInitiatiorName(initiatorIQN);
        CFRelease(initiatorIQN);
    }

    CFStringRef initiatorAlias = iSCSIPLCopyInitiatorAlias();

    if(initiatorAlias) {
        iSCSISetInitiatiorName(initiatorAlias);
        CFRelease(initiatorAlias);
    }

    // Register with launchd so it can manage this daemon
    launch_data_t reg_request = launch_data_new_string(LAUNCH_KEY_CHECKIN);

    // Quit if we are unable to checkin...
    if(!reg_request) {
        fprintf(stderr,"Failed to checkin with launchd.\n");
        goto ERROR_LAUNCH_DATA;
    }

    launch_data_t reg_response = launch_msg(reg_request);

    // Ensure registration was successful
    if((launch_data_get_type(reg_response) == LAUNCH_DATA_ERRNO)) {
        fprintf(stderr,"Failed to checkin with launchd.\n");
        goto ERROR_NO_SOCKETS;
    }

    // Grab label and socket dictionary from daemon's property list
    launch_data_t label = launch_data_dict_lookup(reg_response,LAUNCH_JOBKEY_LABEL);
    launch_data_t sockets = launch_data_dict_lookup(reg_response,LAUNCH_JOBKEY_SOCKETS);

    if(!label || !sockets) {
        fprintf(stderr,"Could not find socket ");
        goto ERROR_NO_SOCKETS;
    }

    launch_data_t listen_socket_array = launch_data_dict_lookup(sockets,"iscsid");

    if(!listen_socket_array || launch_data_array_get_count(listen_socket_array) == 0)
        goto ERROR_NO_SOCKETS;

    // Grab handle to socket we want to listen on...
    launch_data_t listen_socket = launch_data_array_get_index(listen_socket_array,0);

    if(!iSCSIDRegisterForPowerEvents())
        goto ERROR_PWR_MGMT_FAIL;

    // Create a socket that will
    CFSocketRef socket = CFSocketCreateWithNative(kCFAllocatorDefault,
                                                  launch_data_get_fd(listen_socket),
                                                  kCFSocketReadCallBack,
                                                  iSCSIDProcessIncomingRequest,0);

    // Runloop sources associated with socket events of connected clients
    CFRunLoopSourceRef clientSockSource = CFSocketCreateRunLoopSource(kCFAllocatorDefault,socket,0);
    CFRunLoopAddSource(CFRunLoopGetMain(),clientSockSource,kCFRunLoopDefaultMode);

    CFRunLoopRun();
    
    // Deregister for power
    iSCSIDDeregisterForPowerEvents();
    
    launch_data_free(reg_response);
    return 0;
    
    // TODO: verify that launch data is freed under all possible execution paths
    
ERROR_PWR_MGMT_FAIL:
ERROR_NO_SOCKETS:
    launch_data_free(reg_response);
    
ERROR_LAUNCH_DATA:
    
    return ENOTSUP;
}

