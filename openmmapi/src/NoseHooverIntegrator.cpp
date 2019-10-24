/* -------------------------------------------------------------------------- *
 *                                   OpenMM                                   *
 * -------------------------------------------------------------------------- *
 * This is part of the OpenMM molecular simulation toolkit originating from   *
 * Simbios, the NIH National Center for Physics-Based Simulation of           *
 * Biological Structures at Stanford, funded under the NIH Roadmap for        *
 * Medical Research, grant U54 GM072970. See https://simtk.org.               *
 *                                                                            *
 * Portions copyright (c) 2019 Stanford University and the Authors.           *
 * Authors: Andreas Krämer and Andrew C. Simmonett                            *
 * Contributors:                                                              *
 *                                                                            *
 * Permission is hereby granted, free of charge, to any person obtaining a    *
 * copy of this software and associated documentation files (the "Software"), *
 * to deal in the Software without restriction, including without limitation  *
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,   *
 * and/or sell copies of the Software, and to permit persons to whom the      *
 * Software is furnished to do so, subject to the following conditions:       *
 *                                                                            *
 * The above copyright notice and this permission notice shall be included in *
 * all copies or substantial portions of the Software.                        *
 *                                                                            *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    *
 * THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,    *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR      *
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE  *
 * USE OR OTHER DEALINGS IN THE SOFTWARE.                                     *
 * -------------------------------------------------------------------------- */

#include "openmm/NoseHooverIntegrator.h"
#include "openmm/Context.h"
#include "openmm/Force.h"
#include "openmm/System.h"
#include "openmm/internal/NoseHooverChain.h"
#include "openmm/OpenMMException.h"
#include "openmm/CMMotionRemover.h"
#include "openmm/internal/ContextImpl.h"
#include "openmm/internal/AssertionUtilities.h"
#include "openmm/kernels.h"
#include <iostream>
#include <string>
#include <algorithm>

using namespace OpenMM;
using std::string;
using std::vector;

NoseHooverIntegrator::NoseHooverIntegrator(double stepSize):
    forcesAreValid(false)
{
    setStepSize(stepSize);
    setConstraintTolerance(1e-5);
}
NoseHooverIntegrator::NoseHooverIntegrator(double temperature, int collisionFrequnency, double stepSize,
                                           int chainLength, int numMTS, int numYoshidaSuzuki) : forcesAreValid(false) {
    setStepSize(stepSize);
    setConstraintTolerance(1e-5);
    addThermostat(temperature, collisionFrequnency, chainLength, numMTS, numYoshidaSuzuki);
}

NoseHooverIntegrator::~NoseHooverIntegrator() {}

std::pair<double, double> NoseHooverIntegrator::propagateChain(std::pair<double, double> kineticEnergy, int chainID) {
    return nhcKernel.getAs<NoseHooverChainKernel>().propagateChain(*context, noseHooverChains.at(chainID), kineticEnergy, getStepSize());
}


int NoseHooverIntegrator::addThermostat(double temperature, double collisionFrequency,
                                        int chainLength, int numMTS, int numYoshidaSuzuki) {

    return addSubsystemThermostat(std::vector<int>(), std::vector<std::pair<int, int>>(), temperature,
                                  collisionFrequency, temperature, collisionFrequency, chainLength, numMTS, numYoshidaSuzuki);
}

int NoseHooverIntegrator::addSubsystemThermostat(const std::vector<int>& thermostatedParticles,
                                                 const std::vector< std::pair< int, int> > &thermostatedPairs,
                                                 double temperature, double collisionFrequency,
                                                 double relativeTemperature, double relativeCollisionFrequency,
                                                 int chainLength, int numMTS, int numYoshidaSuzuki) {
    auto data = ThermostatData(thermostatedParticles, thermostatedPairs, temperature,
                               relativeTemperature, collisionFrequency,
                               relativeCollisionFrequency, chainLength, numMTS, numYoshidaSuzuki);
    thermostatData.push_back(data);
    return thermostatData.size() - 1;
}


void NoseHooverIntegrator::createThermostats(const System &system) {

    for (const auto &thermostat : thermostatData) {

        // figure out the number of DOFs
        int nDOF = 3*(thermostat.thermostatedParticles.size() + thermostat.thermostatedPairs.size());
        for (int constraintNum = 0; constraintNum < system.getNumConstraints(); constraintNum++) {
            int particle1, particle2;
            double distance;
            system.getConstraintParameters(constraintNum, particle1, particle2, distance);
            bool particle1_in_thermostatedParticles = ((std::find(thermostat.thermostatedParticles.begin(),
                                                                  thermostat.thermostatedParticles.end(), particle1)
                                                                    != thermostat.thermostatedParticles.end())) ||
                                                      (std::find_if(thermostat.thermostatedPairs.begin(),
                                                                    thermostat.thermostatedPairs.end(),
                                                                    [&particle1](const std::pair<int, int>& pair){
                                                                           return pair.first == particle1 || pair.second == particle1;})
                                                                      != thermostat.thermostatedPairs.end());
            bool particle2_in_thermostatedParticles = ((std::find(thermostat.thermostatedParticles.begin(),
                                                                  thermostat.thermostatedParticles.end(), particle2)
                                                                    != thermostat.thermostatedParticles.end())) ||
                                                      (std::find_if(thermostat.thermostatedPairs.begin(),
                                                                    thermostat.thermostatedPairs.end(),
                                                                    [&particle2](const std::pair<int, int>& pair){
                                                                           return pair.first == particle2 || pair.second == particle2;})
                                                                      != thermostat.thermostatedPairs.end());
            if ((system.getParticleMass(particle1) > 0) && (system.getParticleMass(particle2) > 0)){
                if ((particle1_in_thermostatedParticles && !particle2_in_thermostatedParticles) ||
                     (!particle1_in_thermostatedParticles && particle2_in_thermostatedParticles)){
                    throw OpenMMException("Cannot add only one of particles " + std::to_string(particle1) + " and " + std::to_string(particle2)
                                            + " to NoseHooverChain, because they are connected by a constraint.");
                }
                if (particle1_in_thermostatedParticles && particle2_in_thermostatedParticles){
                    nDOF -= 1;
                }
            }
        }

        // remove 3 degrees of freedom from thermostats that act on absolute motions
        int numForces = system.getNumForces();
        if (thermostat.thermostatedPairs.size() == 0){
            for (int forceNum = 0; forceNum < numForces; ++forceNum) {
                if (dynamic_cast<const CMMotionRemover*>(&system.getForce(forceNum))) nDOF -= 3;
            }
        }

        // create and add new chain
        int chainID = noseHooverChains.size();
        auto chain = NoseHooverChain(thermostat.temperature, thermostat.relativeTemperature,
                                     thermostat.collisionFrequency, thermostat.relativeCollisionFrequency,
                                     nDOF, thermostat.chainLength, thermostat.numMTS,
                                     thermostat.numYoshidaSuzuki, chainID,
                                     thermostat.thermostatedParticles, thermostat.thermostatedPairs);
        noseHooverChains.push_back(chain);

    }


    for (int chain1 = 0; chain1 < noseHooverChains.size(); ++chain1){
        const auto& nhc = noseHooverChains[chain1];

       // make sure that thermostats do not overlap
        for (int chain2 = 0; chain2 < chain1; ++chain2){
            const auto& other_nhc = noseHooverChains[chain2];
            for (auto &particle: nhc.getThermostatedAtoms()){
                bool isParticleInOtherChain = (std::find(other_nhc.getThermostatedAtoms().begin(),
                                                         other_nhc.getThermostatedAtoms().end(),
                                                         particle) != other_nhc.getThermostatedAtoms().end()) ||
                                              (std::find_if(other_nhc.getThermostatedPairs().begin(),
                                                            other_nhc.getThermostatedPairs().end(),
                                               [&particle](const std::pair<int, int>& pair){ return pair.first == particle || pair.second == particle;})
                                                  != other_nhc.getThermostatedPairs().end());
                if (isParticleInOtherChain){
                    throw OpenMMException("Found particle " + std::to_string(particle) + "in a different NoseHooverChain, "
                                          "but particles can only be thermostated by one thermostat.");
                }
            }
            for (auto &pair: nhc.getThermostatedPairs()){
                bool isParticleInOtherChain = (std::find(other_nhc.getThermostatedAtoms().begin(),
                                                         other_nhc.getThermostatedAtoms().end(),
                                                         pair.first) != other_nhc.getThermostatedAtoms().end()) ||
                                              (std::find(other_nhc.getThermostatedAtoms().begin(),
                                                         other_nhc.getThermostatedAtoms().end(),
                                                         pair.second) != other_nhc.getThermostatedAtoms().end()) ||
                                              (std::find_if(other_nhc.getThermostatedPairs().begin(),
                                                            other_nhc.getThermostatedPairs().end(),
                                               [&pair](const std::pair<int, int>& other_pair){
                                                        return pair.first == other_pair.first || pair.first == other_pair.second ||
                                                               pair.second == other_pair.first || pair.second == other_pair.second;})
                                                  != other_nhc.getThermostatedPairs().end());
                if (isParticleInOtherChain){
                    throw OpenMMException("Found pair " + std::to_string(pair.first) + "," +
                                          std::to_string(pair.second) + " in a different NoseHooverChain, "
                                          "but particles can only be thermostated by one thermostat.");
                }
            }
        }

        // make sure that massless particles are not thermostated
        for(auto particle: nhc.getThermostatedAtoms()){
            double mass = system.getParticleMass(particle);
            if (mass < 1e-8) {
                throw OpenMMException("Found a particle with no mass (" + std::to_string(particle) + ") in a thermostat. Massless particles cannot be thermostated.");
            }
        }
    }
}

double NoseHooverIntegrator::getTemperature(int chainID) const {
    ASSERT_VALID_INDEX(chainID, noseHooverChains);
    return noseHooverChains[chainID].getDefaultTemperature();
}

void NoseHooverIntegrator::setTemperature(double temperature, int chainID){
    ASSERT_VALID_INDEX(chainID, noseHooverChains);
    noseHooverChains[chainID].setDefaultTemperature(temperature);

}

double NoseHooverIntegrator::getRelativeTemperature(int chainID) const {
    ASSERT_VALID_INDEX(chainID, noseHooverChains);
    return noseHooverChains[chainID].getDefaultRelativeTemperature();
}

void NoseHooverIntegrator::setRelativeTemperature(double temperature, int chainID){
    ASSERT_VALID_INDEX(chainID, noseHooverChains);
    noseHooverChains[chainID].setDefaultRelativeTemperature(temperature);

}

double NoseHooverIntegrator::getCollisionFrequency(int chainID) const {
    ASSERT_VALID_INDEX(chainID, noseHooverChains);
    return noseHooverChains[chainID].getDefaultCollisionFrequency();
}

void NoseHooverIntegrator::setCollisionFrequency(double frequency, int chainID){
    ASSERT_VALID_INDEX(chainID, noseHooverChains);
    noseHooverChains[chainID].setDefaultCollisionFrequency(frequency);
}

double NoseHooverIntegrator::getRelativeCollisionFrequency(int chainID) const {
    ASSERT_VALID_INDEX(chainID, noseHooverChains);
    return noseHooverChains[chainID].getDefaultRelativeCollisionFrequency();
}

void NoseHooverIntegrator::setRelativeCollisionFrequency(double frequency, int chainID){
    ASSERT_VALID_INDEX(chainID, noseHooverChains);
    noseHooverChains[chainID].setDefaultRelativeCollisionFrequency(frequency);
}

double NoseHooverIntegrator::computeKineticEnergy() {
    double kE = 0.0;
    if(noseHooverChains.size() > 0) {
        for (const auto &nhc: noseHooverChains){
            kE += nhcKernel.getAs<NoseHooverChainKernel>().computeMaskedKineticEnergy(*context, nhc, true).first;
        }
    } else {
        kE = vvKernel.getAs<IntegrateVelocityVerletStepKernel>().computeKineticEnergy(*context, *this);
    }
    return kE;
}

double NoseHooverIntegrator::computeHeatBathEnergy() {
    double energy = 0;
    for(auto &nhc : noseHooverChains) {
        energy += nhcKernel.getAs<NoseHooverChainKernel>().computeHeatBathEnergy(*context, nhc);
    }
    return energy;
}

void NoseHooverIntegrator::initialize(ContextImpl& contextRef) {
    if (owner != NULL && &contextRef.getOwner() != owner)
        throw OpenMMException("This Integrator is already bound to a context");

    context = &contextRef;
    owner = &contextRef.getOwner();
    vvKernel = context->getPlatform().createKernel(IntegrateVelocityVerletStepKernel::Name(), contextRef);
    vvKernel.getAs<IntegrateVelocityVerletStepKernel>().initialize(contextRef.getSystem(), *this);
    nhcKernel = context->getPlatform().createKernel(NoseHooverChainKernel::Name(), contextRef);
    nhcKernel.getAs<NoseHooverChainKernel>().initialize();
    forcesAreValid = false;

    // check for drude particles and build the Nose-Hoover Chains
    const System& system = context->getSystem();
    for (auto& thermostat: thermostatData){
        // if there are no thermostated particles or pairs in the lists this is a regular thermostat for the whole (non-Drude) system
        if ( (thermostat.thermostatedParticles.size() == 0) && (thermostat.thermostatedPairs.size() == 0) ){
            for(int particle = 0; particle < system.getNumParticles(); ++particle) {
                double mass = system.getParticleMass(particle);
                if ( (mass > 0) && (mass < 0.8) ){
                    std::cout << "Warning: Found particles with mass between 0.0 and 0.8 dalton. Did you mean to make a DrudeNoseHooverIntegrator instead? "
                                 "The thermostat you are about to use will not treat these particles as Drude particles!" << std::endl;
                }
                if(system.getParticleMass(particle) > 0) {
                    thermostat.thermostatedParticles.push_back(particle);
                }
            }
        }
    }

    createThermostats(system);
}

void NoseHooverIntegrator::cleanup() {
    vvKernel = Kernel();
    nhcKernel = Kernel();
}

vector<string> NoseHooverIntegrator::getKernelNames() {
    std::vector<std::string> names;
    names.push_back(NoseHooverChainKernel::Name());
    names.push_back(IntegrateVelocityVerletStepKernel::Name());
    return names;
}

void NoseHooverIntegrator::step(int steps) {
    if (context == NULL)
        throw OpenMMException("This Integrator is not bound to a context!");
    std::pair<double, double> scale, kineticEnergy;
    for (int i = 0; i < steps; ++i) {
        context->updateContextState();
        for(auto &nhc : noseHooverChains) {
            kineticEnergy = nhcKernel.getAs<NoseHooverChainKernel>().computeMaskedKineticEnergy(*context, nhc, false);
            scale = nhcKernel.getAs<NoseHooverChainKernel>().propagateChain(*context, nhc, kineticEnergy, getStepSize());
            nhcKernel.getAs<NoseHooverChainKernel>().scaleVelocities(*context, nhc, scale);
        }
        vvKernel.getAs<IntegrateVelocityVerletStepKernel>().execute(*context, *this, forcesAreValid);
        for(auto &nhc : noseHooverChains) {
            kineticEnergy = nhcKernel.getAs<NoseHooverChainKernel>().computeMaskedKineticEnergy(*context, nhc, false);
            scale = nhcKernel.getAs<NoseHooverChainKernel>().propagateChain(*context, nhc, kineticEnergy, getStepSize());
            nhcKernel.getAs<NoseHooverChainKernel>().scaleVelocities(*context, nhc, scale);
        }
    }
}
