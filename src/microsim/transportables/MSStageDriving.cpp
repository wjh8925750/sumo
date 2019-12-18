/****************************************************************************/
// Eclipse SUMO, Simulation of Urban MObility; see https://eclipse.org/sumo
// Copyright (C) 2001-2019 German Aerospace Center (DLR) and others.
// This program and the accompanying materials
// are made available under the terms of the Eclipse Public License v2.0
// which accompanies this distribution, and is available at
// http://www.eclipse.org/legal/epl-v20.html
// SPDX-License-Identifier: EPL-2.0
/****************************************************************************/
/// @file    MSStageDriving.cpp
/// @author  Melanie Weber
/// @author  Andreas Kendziorra
/// @author  Michael Behrisch
/// @date    Thu, 12 Jun 2014
///
// The common superclass for modelling transportable objects like persons and containers
/****************************************************************************/


// ===========================================================================
// included modules
// ===========================================================================
#include <config.h>

#include <utils/common/StringTokenizer.h>
#include <utils/geom/GeomHelper.h>
#include <utils/vehicle/SUMOVehicleParameter.h>
#include <utils/router/PedestrianRouter.h>
#include <utils/router/IntermodalRouter.h>
#include <microsim/MSEdge.h>
#include <microsim/MSLane.h>
#include <microsim/MSNet.h>
#include <microsim/MSInsertionControl.h>
#include <microsim/MSVehicleControl.h>
#include <microsim/MSStoppingPlace.h>
#include <microsim/transportables/MSPerson.h>
#include <microsim/devices/MSTransportableDevice.h>
#include <microsim/devices/MSDevice_Taxi.h>
#include <microsim/devices/MSDevice_Tripinfo.h>
#include <microsim/transportables/MSTransportableControl.h>
#include <microsim/transportables/MSStageDriving.h>


// ===========================================================================
// method definitions
// ===========================================================================
MSStageDriving::MSStageDriving(const MSEdge* destination,
                               MSStoppingPlace* toStop, const double arrivalPos, const std::vector<std::string>& lines,
                               const std::string& intendedVeh, SUMOTime intendedDepart) :
    MSStage(destination, toStop, arrivalPos, MSStageType::DRIVING),
    myLines(lines.begin(), lines.end()),
    myVehicle(nullptr),
    myVehicleID("NULL"),
    myVehicleVClass(SVC_IGNORING),
    myVehicleDistance(-1.),
    myWaitingSince(-1),
    myWaitingEdge(nullptr),
    myStopWaitPos(Position::INVALID),
    myIntendedVehicleID(intendedVeh),
    myIntendedDepart(intendedDepart) {
}


MSStage*
MSStageDriving::clone() const {
    return new MSStageDriving(myDestination, myDestinationStop, myArrivalPos, std::vector<std::string>(myLines.begin(), myLines.end()),
                              myIntendedVehicleID, myIntendedDepart);
}


MSStageDriving::~MSStageDriving() {}


const MSEdge*
MSStageDriving::getEdge() const {
    if (myVehicle != nullptr) {
        if (myVehicle->getLane() != nullptr) {
            return &myVehicle->getLane()->getEdge();
        }
        return myVehicle->getEdge();
    }
    return myWaitingEdge;
}


const MSEdge*
MSStageDriving::getFromEdge() const {
    return myWaitingEdge;
}


double
MSStageDriving::getEdgePos(SUMOTime /* now */) const {
    if (isWaiting4Vehicle()) {
        return myWaitingPos;
    }
    // vehicle may already have passed the lane (check whether this is correct)
    return MIN2(myVehicle->getPositionOnLane(), getEdge()->getLength());
}


Position
MSStageDriving::getPosition(SUMOTime /* now */) const {
    if (isWaiting4Vehicle()) {
        if (myStopWaitPos != Position::INVALID) {
            return myStopWaitPos;
        }
        return getEdgePosition(myWaitingEdge, myWaitingPos,
                               ROADSIDE_OFFSET * (MSNet::getInstance()->lefthand() ? -1 : 1));
    }
    return myVehicle->getPosition();
}


double
MSStageDriving::getAngle(SUMOTime /* now */) const {
    if (!isWaiting4Vehicle()) {
        MSVehicle* veh = dynamic_cast<MSVehicle*>(myVehicle);
        if (veh != nullptr) {
            return veh->getAngle();
        } else {
            return 0;
        }
    }
    return getEdgeAngle(myWaitingEdge, myWaitingPos) + M_PI / 2. * (MSNet::getInstance()->lefthand() ? -1 : 1);
}


std::string
MSStageDriving::getStageDescription(const bool isPerson) const {
    return isWaiting4Vehicle() ? "waiting for " + joinToString(myLines, ",") : (isPerson ? "driving" : "transport");
}


std::string
MSStageDriving::getStageSummary(const bool isPerson) const {
    const std::string dest = (getDestinationStop() == nullptr ?
                              " edge '" + getDestination()->getID() + "'" :
                              " stop '" + getDestinationStop()->getID() + "'" + (
                                  getDestinationStop()->getMyName() != "" ? " (" + getDestinationStop()->getMyName() + ")" : ""));
    const std::string intended = myIntendedVehicleID != "" ?
                                 " (vehicle " + myIntendedVehicleID + " at time " + time2string(myIntendedDepart) + ")" :
                                 "";
    const std::string modeName = isPerson ? "driving" : "transported";
    return isWaiting4Vehicle() ?
           "waiting for " + joinToString(myLines, ",") + intended + " then " + modeName + " to " + dest :
           modeName + " to " + dest;
}


void
MSStageDriving::proceed(MSNet* net, MSTransportable* transportable, SUMOTime now, MSStage* previous) {
    const MSStoppingPlace* start = (previous->getStageType() == MSStageType::TRIP
                                    ? previous->getOriginStop()
                                    : previous->getDestinationStop());
    myWaitingSince = now;
    const bool isPerson = transportable->isPerson();
    if (transportable->getParameter().departProcedure == DEPART_TRIGGERED
            && transportable->getNumRemainingStages() == transportable->getNumStages() - 1) {
        // we are the first real stage (stage 0 is WAITING_FOR_DEPART)
        const std::string vehID = *myLines.begin();
        SUMOVehicle* startVeh = net->getVehicleControl().getVehicle(vehID);
        if (startVeh == nullptr) {
            throw ProcessError("Vehicle '" + vehID + "' not found for triggered departure of " +
                               (isPerson ? "person" : "container") + " '" + transportable->getID() + "'.");
        }
        setVehicle(startVeh);
        myVehicle->addTransportable(transportable);
        return;
    }
    if (start != nullptr) {
        // the arrival stop may have an access point
        myWaitingEdge = &start->getLane().getEdge();
        myStopWaitPos = start->getWaitPosition(transportable);
        myWaitingPos = start->getWaitingPositionOnLane(transportable);
    } else {
        myWaitingEdge = previous->getEdge();
        myStopWaitPos = Position::INVALID;
        myWaitingPos = previous->getEdgePos(now);
    }
    SUMOVehicle* const availableVehicle = myWaitingEdge->getWaitingVehicle(transportable, myWaitingPos);
    const bool triggered = availableVehicle != nullptr &&
                           ((isPerson && availableVehicle->getParameter().departProcedure == DEPART_TRIGGERED) ||
                            (!isPerson && availableVehicle->getParameter().departProcedure == DEPART_CONTAINER_TRIGGERED));
    if (triggered && !availableVehicle->hasDeparted()) {
        setVehicle(availableVehicle);
        myVehicle->addTransportable(transportable);
        net->getInsertionControl().add(myVehicle);
        myWaitingEdge->removeWaiting(myVehicle);
        net->getVehicleControl().unregisterOneWaiting(true);
    } else {
        if (isPerson) {
            net->getPersonControl().addWaiting(myWaitingEdge, transportable);
            myWaitingEdge->addPerson(transportable);
            if (getLines().size() == 1 && *getLines().begin() == "taxi") {
                MSDevice_Taxi::addReservation(transportable, now, now, myWaitingEdge, myWaitingPos, getDestination(), myArrivalPos);
            }
        } else {
            net->getContainerControl().addWaiting(myWaitingEdge, transportable);
            myWaitingEdge->addContainer(transportable);
        }
    }
}


void
MSStageDriving::tripInfoOutput(OutputDevice& os, const MSTransportable* const transportable) const {
    const SUMOTime now = MSNet::getInstance()->getCurrentTimeStep();
    const SUMOTime departed = myDeparted >= 0 ? myDeparted : now;
    const SUMOTime waitingTime = myWaitingSince >= 0 ? departed - myWaitingSince : -1;
    const SUMOTime duration = myArrived - myDeparted;
    MSDevice_Tripinfo::addRideTransportData(transportable->isPerson(), myVehicleDistance, duration, myVehicleVClass, myVehicleLine, waitingTime);
    os.openTag(transportable->isPerson() ? "ride" : "transport");
    os.writeAttr("waitingTime", waitingTime >= 0 ? time2string(waitingTime) : "-1");
    os.writeAttr("vehicle", myVehicleID);
    os.writeAttr("depart", myDeparted >= 0 ? time2string(myDeparted) : "-1");
    os.writeAttr("arrival", myArrived >= 0 ? time2string(myArrived) : "-1");
    os.writeAttr("arrivalPos", toString(myArrivalPos));
    os.writeAttr("duration", myArrived >= 0 ? time2string(duration) :
                 (myDeparted >= 0 ? time2string(now - myDeparted) : "-1"));
    os.writeAttr("routeLength", myVehicleDistance);
    os.closeTag();
}


void
MSStageDriving::routeOutput(const bool isPerson, OutputDevice& os, const bool withRouteLength) const {
    os.openTag(isPerson ? "ride" : "transport");
    if (getFromEdge() != nullptr) {
        os.writeAttr(SUMO_ATTR_FROM, getFromEdge()->getID());
    }
    os.writeAttr(SUMO_ATTR_TO, getDestination()->getID());
    std::string comment = "";
    if (myDestinationStop != nullptr) {
        os.writeAttr(SUMO_ATTR_BUS_STOP, myDestinationStop->getID());
        if (myDestinationStop->getMyName() != "") {
            comment = " <!-- " + StringUtils::escapeXML(myDestinationStop->getMyName(), true) + " -->";
        }
    }
    os.writeAttr(SUMO_ATTR_LINES, myLines);
    if (myIntendedVehicleID != "") {
        os.writeAttr(SUMO_ATTR_INTENDED, myIntendedVehicleID);
    }
    if (myIntendedDepart >= 0) {
        os.writeAttr(SUMO_ATTR_DEPART, time2string(myIntendedDepart));
    }
    if (withRouteLength) {
        os.writeAttr("routeLength", myVehicleDistance);
    }
    os.closeTag(comment);
}


bool
MSStageDriving::isWaitingFor(const SUMOVehicle* vehicle) const {
    return (myLines.count(vehicle->getID()) > 0
            || myLines.count(vehicle->getParameter().line) > 0
            || (myLines.count("ANY") > 0 && (
                    myDestinationStop == nullptr
                    ? vehicle->stopsAtEdge(myDestination)
                    : vehicle->stopsAt(myDestinationStop))));
}


bool
MSStageDriving::isWaiting4Vehicle() const {
    return myVehicle == nullptr;
}


SUMOTime
MSStageDriving::getWaitingTime(SUMOTime now) const {
    return isWaiting4Vehicle() ? now - myWaitingSince : 0;
}


double
MSStageDriving::getSpeed() const {
    return isWaiting4Vehicle() ? 0 : myVehicle->getSpeed();
}


ConstMSEdgeVector
MSStageDriving::getEdges() const {
    ConstMSEdgeVector result;
    result.push_back(getFromEdge());
    result.push_back(getDestination());
    return result;
}


const std::string
MSStageDriving::setArrived(MSNet* net, MSTransportable* transportable, SUMOTime now) {
    MSStage::setArrived(net, transportable, now);
    if (myVehicle != nullptr) {
        // distance was previously set to driven distance upon embarking
        myVehicleDistance = myVehicle->getRoute().getDistanceBetween(
                                myVehicle->getDepartPos(), myVehicle->getPositionOnLane(),
                                myVehicle->getRoute().begin(),  myVehicle->getCurrentRouteEdge()) - myVehicleDistance;
        if (myVehicle->isStopped()) {
            myArrivalPos = myVehicle->getPositionOnLane();
        }
    } else {
        myVehicleDistance = -1.;
    }
    return "";
}


void
MSStageDriving::setVehicle(SUMOVehicle* v) {
    myVehicle = v;
    myVehicleID = v->getID();
    myVehicleLine = v->getParameter().line;
    myVehicleVClass = v->getVClass();
    myVehicleDistance = myVehicle->getRoute().getDistanceBetween(
                            myVehicle->getDepartPos(), myVehicle->getPositionOnLane(),
                            myVehicle->getRoute().begin(),  myVehicle->getCurrentRouteEdge());
}

void
MSStageDriving::abort(MSTransportable* t) {
    if (myVehicle != nullptr) {
        // jumping out of a moving vehicle!
        myVehicle->removeTransportable(t);
    } else {
        MSTransportableControl& tc = (t->isPerson() ?
                                      MSNet::getInstance()->getPersonControl() :
                                      MSNet::getInstance()->getContainerControl());
        tc.abortWaitingForVehicle(t);
    }
}


std::string
MSStageDriving::getWaitingDescription() const {
    return isWaiting4Vehicle() ? ("waiting for " + joinToString(myLines, ",")
                                  + " at " + (myDestinationStop == nullptr
                                          ? ("edge '" + myWaitingEdge->getID() + "'")
                                          : ("busStop '" + myDestinationStop->getID() + "'"))
                                 ) : "";
}


/****************************************************************************/
