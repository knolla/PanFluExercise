#include "StochasticSEATIRD.h"
#include "../../Parameters.h"
#include "../random.h"
#include "../../Stockpile.h"
#include "../../StockpileNetwork.h"
#include "../../PriorityGroup.h"
#include "../../PriorityGroupSelections.h"
#include "../../Npi.h"
#include "../../log.h"
#include <boost/bind.hpp>

const int StochasticSEATIRD::numAgeGroups_ = 5;
const int StochasticSEATIRD::numRiskGroups_ = 2;
const int StochasticSEATIRD::numVaccinatedGroups_ = 2;

StochasticSEATIRD::StochasticSEATIRD()
{
    put_flog(LOG_DEBUG, "");

    // defaults
    cachedTime_ = -1;

    // create other required variables for this model
    newVariable("asymptomatic");
    newVariable("treatable");
    newVariable("infectious");
    newVariable("recovered");
    newVariable("deceased");

    // the "treated" variable keeps tracks of those treated with antivirals
    newVariable("treated");

    // need to keep track of number treated each day
    newVariable("treated (daily)");

    // need to keep track of number ineffectively treated each day
    newVariable("treated (ineffective daily)");

    // need to keep track of number vaccinated each day
    newVariable("vaccinated (daily)");

    // derived variables
    derivedVariables_["All infected"] = boost::bind(&StochasticSEATIRD::getDerivedVarInfected, this, _1, _2, _3);
    derivedVariables_["vaccinated in lag period"] = boost::bind(&StochasticSEATIRD::getDerivedVarPopulationInVaccineLatencyPeriod, this, _1, _2, _3);
    derivedVariables_["vaccinated effective"] = boost::bind(&StochasticSEATIRD::getDerivedVarPopulationEffectiveVaccines, this, _1, _2, _3);
    derivedVariables_["ILI reports"] = boost::bind(&StochasticSEATIRD::getDerivedVarILI, this, _1, _2, _3);

    // initialize ILI
    iliProviders_ = iliInit();

    // initialize ILI values to zero
    std::vector<float> iliValues;

    for(unsigned int i=0; i<getNumNodes(); i++)
    {
        iliValues.push_back(0.);
    }

    iliValues_.push_back(iliValues);

    // initialize start time to 0
    time_ = 0;
    now_ = 0.;

    // initiate random number generator
    gsl_rng_env_setup();
    randGenerator_ = gsl_rng_alloc(gsl_rng_default);
}

StochasticSEATIRD::~StochasticSEATIRD()
{
    put_flog(LOG_DEBUG, "");

    gsl_rng_free(randGenerator_);
}

int StochasticSEATIRD::expose(int num, int nodeId, std::vector<int> stratificationValues)
{
    // expose() can be called outside of a simulation before we've simulated any time steps
    if(time_ == 0 && cachedTime_ == -1)
    {
        put_flog(LOG_DEBUG, "precomputing at beginning of simulation");

        // in this case we don't precompute on time_+1 since it doesn't exist yet
        // this will still produce correct results since there's no movement in stratifications
        precompute(0);
    }
    else if(time_ != 0 && cachedTime_ != time_+1)
    {
        put_flog(LOG_WARN, "precomputing during simulation! should not be necessary.");

        precompute(time_+1);
    }

    int numExposed = EpidemicSimulation::expose(num, nodeId, stratificationValues);

    // create events based on these new exposures
    for(int i=0; i<numExposed; i++)
    {
        StochasticSEATIRDSchedule schedule(now_, rand_, stratificationValues);

        initializeContactEvents(schedule, nodeId, stratificationValues);

        // now add event schedules to big queue
        scheduleEventQueues_[nodeId].push(schedule);
    }

    return numExposed;
}

void StochasticSEATIRD::simulate()
{
    // we are simulating from time_ to time_+1
    now_ = (double)time_;

    // base class simulate(): copies variables to new time step (time_+1) and evolves stockpile network
    EpidemicSimulation::simulate();

    // enable this for schedule verification (this is expensive!)
#if 0
    if(verifyScheduleCounts() != true)
    {
        put_flog(LOG_ERROR, "failed verification of schedule counts");
    }
#endif

    // apply treatments

    // create a priority group selection for all of the population, for pure pro-rata treatments
    std::vector<int> stratificationValues(1, STRATIFICATIONS_ALL);
    std::vector<std::vector<int> > stratificationVectorValues(3, stratificationValues);
    boost::shared_ptr<PriorityGroup> priorityGroupAll = boost::shared_ptr<PriorityGroup>(new PriorityGroup("_ALL_", stratificationVectorValues));
    boost::shared_ptr<PriorityGroupSelections> priorityGroupSelectionsAll(new PriorityGroupSelections(std::vector<boost::shared_ptr<PriorityGroup> >(1, priorityGroupAll)));

    // reset number treated for today
    // do this here since we may have multiple treatments in one day
    variables_["treated (daily)"](time_+1, blitz::Range::all(), blitz::Range::all(), blitz::Range::all(), blitz::Range::all()) = 0.;
    variables_["treated (ineffective daily)"](time_+1, blitz::Range::all(), blitz::Range::all(), blitz::Range::all(), blitz::Range::all()) = 0.;
    variables_["vaccinated (daily)"](time_+1, blitz::Range::all(), blitz::Range::all(), blitz::Range::all(), blitz::Range::all()) = 0.;

    // apply treatments to priority group selections; then remaining to the entire population
    applyAntiviralsToPriorityGroupSelections(g_parameters.getAntiviralPriorityGroupSelections());
    applyAntiviralsToPriorityGroupSelections(priorityGroupSelectionsAll);

    applyVaccinesToPriorityGroupSelections(g_parameters.getVaccinePriorityGroupSelections());
    applyVaccinesToPriorityGroupSelections(priorityGroupSelectionsAll);

    // pre-compute some frequently used values
    // this should be done after applyVaccines() since individuals may be changing stratifications
    // we operate on the new time step (time_+1) to capture such stratification changes
    precompute(time_+1);

    // process events for each node
    for(unsigned int i=0; i<nodeIds_.size(); i++)
    {
        int nodeId = nodeIds_[i];

        while(scheduleEventQueues_[nodeId].empty() != true && scheduleEventQueues_[nodeId].top().getTopEvent().time < (double)time_+1.)
        {
            // pop the schedule off the schedule queue
            StochasticSEATIRDSchedule schedule = scheduleEventQueues_[nodeId].top();
            scheduleEventQueues_[nodeId].pop();

            // make sure schedule isn't empty or canceled (it could be canceled from applying treatments, for example)
            if(schedule.empty() != true && schedule.canceled() != true)
            {
                // pop the event off the schedule's event queue
                StochasticSEATIRDEvent event = schedule.getTopEvent();
                schedule.popTopEvent();

                // process the event
                now_ = event.time;

                processEvent(nodeId, event);

                // re-insert the schedule back into the schedule queue
                // it will be sorted corresponding to its next event
                if(schedule.empty() != true)
                {
                    scheduleEventQueues_[nodeId].push(schedule);
                }
            }
        }
    }

    // current event time is now the end of the current day
    now_ = (double)time_ + 1.;

    // travel between nodes
    travel();

    // ILI
    std::vector<float> infectious;
    std::vector<float> population;

    std::vector<int> nodeIds = getNodeIds();

    for(unsigned int i=0; i<nodeIds.size(); i++)
    {
        infectious.push_back(getDerivedVarInfected(time_, nodeIds[i]));
        population.push_back(getPopulation(nodeIds[i]));
    }

    std::vector<float> iliValues = iliView(infectious, population, iliProviders_);

    iliValues_.push_back(iliValues);

    // increment current time
    time_++;
}

float StochasticSEATIRD::getDerivedVarInfected(int time, int nodeId, std::vector<int> stratificationValues)
{
    float infected = 0.;
    infected += getValue("asymptomatic", time, nodeId, stratificationValues);
    infected += getValue("treatable", time, nodeId, stratificationValues);
    infected += getValue("infectious", time, nodeId, stratificationValues);

    return infected;
}

float StochasticSEATIRD::getDerivedVarPopulationInVaccineLatencyPeriod(int time, int nodeId, std::vector<int> stratificationValues)
{
    // should match the other getPopulationInVaccineLatencyPeriod() method below

    // no need to limit to vaccinated stratification, since non-vaccinated will always be zero for this variable

    int vaccineLatencyPeriod = g_parameters.getVaccineLatencyPeriod();

    float total = 0;

    // with these inequalities, a 0 day latency period will always return 0, as expected
    for(int t=time; t>=0 && t>(time - vaccineLatencyPeriod); t--)
    {
        // vaccinated stratification == 1
        total += getValue("vaccinated (daily)", t, nodeId, stratificationValues);
    }

    return total;
}

float StochasticSEATIRD::getDerivedVarPopulationEffectiveVaccines(int time, int nodeId, std::vector<int> stratificationValues)
{
    // vaccinated stratification == 1
    // return 0 if unvaccinated stratification was explicitly specified
    if(stratificationValues.size() >= 3 && (stratificationValues[2] != 1 && stratificationValues[2] != STRATIFICATIONS_ALL))
    {
        return 0.;
    }

    // make sure stratifications size is full and choose vaccinated stratification
    for(unsigned int i=stratificationValues.size(); i<3; i++)
    {
        stratificationValues.push_back(STRATIFICATIONS_ALL);
    }

    stratificationValues[2] = 1;

    return getValue("population", time, nodeId, stratificationValues) - getDerivedVarPopulationInVaccineLatencyPeriod(time, nodeId, stratificationValues);
}

float StochasticSEATIRD::getDerivedVarILI(int time, int nodeId, std::vector<int> stratificationValues)
{
    return iliValues_[time][nodeIdToIndex_[nodeId]] * getPopulation(nodeId);
}

std::vector<Provider> StochasticSEATIRD::getIliProviders()
{
    return iliProviders_;
}

void StochasticSEATIRD::initializeContactEvents(StochasticSEATIRDSchedule &schedule, const int &nodeId, const std::vector<int> &stratificationValues)
{
    // todo: beta should be age-specific considering PHA's
    double beta = g_parameters.getR0() / g_parameters.getBetaScale();

    // todo: should be in parameters
    static double sigma[] = {1.00, 0.98, 0.94, 0.91, 0.66};

    // todo: should be in parameters
    static double contact[5][5] = {    { 45.1228487783,8.7808312353,11.7757947836,6.10114751268,4.02227175596 },
                                { 8.7808312353,41.2889143668,13.3332813497,7.847051289,4.22656343551 },
                                { 11.7757947836,13.3332813497,21.4270155984,13.7392636644,6.92483172729 },
                                { 6.10114751268,7.847051289,13.7392636644,18.0482119252,9.45371062356 },
                                { 4.02227175596,4.22656343551,6.92483172729,9.45371062356,14.0529294262 }   };

    // make sure we have expected stratifications
    if((int)stratifications_[0].size() != StochasticSEATIRD::numAgeGroups_ || (int)stratifications_[1].size() != StochasticSEATIRD::numRiskGroups_ || (int)stratifications_[2].size() != StochasticSEATIRD::numVaccinatedGroups_)
    {
        put_flog(LOG_ERROR, "wrong number of stratifications");
        return;
    }

    // contact events will only be targeted at (age group, risk group)
    // vaccinated status changes over time, and these events are all initiated at the point of exposure
    // when the contact event occurs, it will then be determined if the target individual is vaccinated or not
    std::vector<int> toStratificationValues(2);

    for(int a=0; a<StochasticSEATIRD::numAgeGroups_; a++)
    {
        for(int r=0; r<StochasticSEATIRD::numRiskGroups_; r++)
        {
            toStratificationValues[0] = a;
            toStratificationValues[1] = r;

            // fraction of the to group in population; use cached values
            // sum both unvaccinated and vaccinated stratifications
            double toGroupFraction = (populations_(nodeIdToIndex_[nodeId], a, r, 0) + populations_(nodeIdToIndex_[nodeId], a, r, 1))  / populationNodes_(nodeIdToIndex_[nodeId]);

            double contactRate = contact[stratificationValues[0]][a];
            double transmissionRate = beta * contactRate * sigma[a] * toGroupFraction;

            // contacts can occur within this time range
            double TcInit = schedule.getInfectedTMin(); // asymptomatic
            double TcFinal = schedule.getInfectedTMax(); // recovered / deceased

            // the first contact time...
            double Tc = TcInit + random_exponential(transmissionRate, &rand_);

            while(Tc < TcFinal)
            {
                schedule.insertEvent(StochasticSEATIRDEvent(TcInit, Tc, CONTACT, stratificationValues, toStratificationValues));

                TcInit = Tc;
                Tc = TcInit + random_exponential(transmissionRate, &rand_);
            }
        }
    }
}

bool StochasticSEATIRD::processEvent(const int &nodeId, const StochasticSEATIRDEvent &event)
{
    switch(event.type)
    {
        case EtoA:
            // exposed -> asymptomatic
            transition(1, "exposed", "asymptomatic", nodeId, event.fromStratificationValues);
            break;

        case AtoT:
            // asymptomatic -> treatable
            transition(1, "asymptomatic", "treatable", nodeId, event.fromStratificationValues);
            break;
        case AtoR:
            // asymptomatic -> recovered
            transition(1, "asymptomatic", "recovered", nodeId, event.fromStratificationValues);
            break;
        case AtoD:
            // asymptomatic -> deceased
            transition(1, "asymptomatic", "deceased", nodeId, event.fromStratificationValues);
            break;

        case TtoI:
            // treatable -> infectious
            transition(1, "treatable", "infectious", nodeId, event.fromStratificationValues);
            break;
        case TtoR:
            // treatable -> recovered
            transition(1, "treatable", "recovered", nodeId, event.fromStratificationValues);
            break;
        case TtoD:
            // treatable -> deceased
            transition(1, "treatable", "deceased", nodeId, event.fromStratificationValues);
            break;

        case ItoR:
            // infectious -> recovered
            transition(1, "infectious", "recovered", nodeId, event.fromStratificationValues);
            break;
        case ItoD:
            // infectious -> deceased
            transition(1, "infectious", "deceased", nodeId, event.fromStratificationValues);
            break;

        case CONTACT:
            // contact events only target (age group, risk group)
            if(event.toStratificationValues.size() != 2)
            {
                put_flog(LOG_ERROR, "incorrect event.toStratificationValues; size == %i", event.toStratificationValues.size());
                return false;
            }

            // first, see if a Npi stops this contact from happening
            bool npiEffective = Npi::isNpiEffective(g_parameters.getNpis(), nodeId, int(now_), event.fromStratificationValues[0], event.toStratificationValues[0]);

            if(npiEffective == true)
            {
                // the Npis are effective
                break;
            }

            // determine now if the target individual is vaccinated or not
            int ageRiskPopulationSize = int(populations_(nodeIdToIndex_[nodeId], event.toStratificationValues[0], event.toStratificationValues[1], 0) + populations_(nodeIdToIndex_[nodeId], event.toStratificationValues[0], event.toStratificationValues[1], 1));

            // vaccinated stratification == 1
            int ageRiskVaccinatedPopulationSize = int(populations_(nodeIdToIndex_[nodeId], event.toStratificationValues[0], event.toStratificationValues[1], 1));

            // random integer between 1 and ageRiskPopulationSize
            int contact = rand_.randInt(ageRiskPopulationSize - 1) + 1;

            // the vaccinated stratification value
            int v = 0;

            if(ageRiskVaccinatedPopulationSize >= contact)
            {
                // the target individual is vaccinated
                v = 1;

                // only continue if the vaccine is not effective

                // if the individual is still in the vaccine latency period, the vaccine is not effective
                int ageRiskVaccinatedLatencyPopulationSize = getPopulationInVaccineLatencyPeriod(nodeId, event.toStratificationValues[0], event.toStratificationValues[1]);

                if(ageRiskVaccinatedLatencyPopulationSize < contact)
                {
                    // individual is NOT in the vaccine latency period
                    // the vaccine therefore might be effective

                    // todo: should be age-specific
                    double vaccineEffectiveness = g_parameters.getVaccineEffectiveness();

                    if(rand_.rand() <= vaccineEffectiveness)
                    {
                        // the vaccine is effective
                        break;
                    }
                }
            }

            // form the complete toStratificationValues
            std::vector<int> completeToStratificationValues = event.toStratificationValues;
            completeToStratificationValues.push_back(v);

            int targetPopulationSize = int(populations_(nodeIdToIndex_[nodeId], completeToStratificationValues[0], completeToStratificationValues[1], completeToStratificationValues[2]));

            if(event.fromStratificationValues == completeToStratificationValues)
            {
                targetPopulationSize -= 1; // - 1 because randint includes both endpoints
            }

            if(targetPopulationSize > 0)
            {
                // random integer between 1 and targetPopulationSize
                contact = rand_.randInt(targetPopulationSize - 1) + 1;

                if((int)getValue("susceptible", time_+1, nodeId, completeToStratificationValues) >= contact)
                {
                    expose(1, nodeId, completeToStratificationValues);
                }
            }

            break;
    }

    return true;
}

void StochasticSEATIRD::applyAntiviralsToPriorityGroupSelections(boost::shared_ptr<PriorityGroupSelections> priorityGroupSelections)
{
    if(priorityGroupSelections == NULL || priorityGroupSelections->getPriorityGroups().size() == 0)
    {
        put_flog(LOG_DEBUG, "no priority groups in selection");
        return;
    }

    double antiviralEffectiveness = g_parameters.getAntiviralEffectiveness();
    double antiviralAdherence = g_parameters.getAntiviralAdherence();
    double antiviralCapacity = g_parameters.getAntiviralCapacity();

    // treatments for each node
    std::vector<int> nodeIds = getNodeIds();

    for(unsigned int i=0; i<nodeIds.size(); i++)
    {
        boost::shared_ptr<Stockpile> stockpile = getStockpileNetwork()->getNodeStockpile(nodeIds[i]);

        // do nothing if no stockpile is found
        if(stockpile == NULL)
        {
            continue;
        }

        // available antivirals stockpile
        int stockpileAmount = stockpile->getNum(time_+1, STOCKPILE_ANTIVIRALS);

        // do nothing if we have no available stockpile
        if(stockpileAmount == 0)
        {
            continue;
        }

        // the total populations below correspond to the priority group selections

        // determine total number of adherent treatable
        float totalTreatable = getValue("treatable", time_+1, nodeIds[i], priorityGroupSelections->getStratificationValuesSet()) - getValue("treated (ineffective daily)", time_+1, nodeIds[i], priorityGroupSelections->getStratificationValuesSet());

        // do nothing if this population is zero
        if(totalTreatable <= 0.)
        {
            continue;
        }

        // since we fix the treatable period to one day, we can simplify our adherence calculations...
        float totalAdherentTreatable = antiviralAdherence * totalTreatable;

        // we will use all of our available stockpile (subject to capacity constraint) to treat the adherent treatable population
        int stockpileAmountUsed = stockpileAmount;

        if(stockpileAmountUsed > (int)totalAdherentTreatable)
        {
            stockpileAmountUsed = (int)totalAdherentTreatable;
        }

        // capacity corresponds to total population, not just for these priority group selections
        float capacityTotalPopulation = getValue("population", time_+1, nodeIds[i]);

        // consider capacity used in previous treatments on this day
        float todayUsedCapacity = blitz::sum(variables_["treated (daily)"](time_+1, nodeIdToIndex_[nodeIds[i]], blitz::Range::all(), blitz::Range::all(), blitz::Range::all()));

        if(stockpileAmountUsed > (int)(antiviralCapacity * capacityTotalPopulation - todayUsedCapacity))
        {
            stockpileAmountUsed = (int)(antiviralCapacity * capacityTotalPopulation - todayUsedCapacity);
        }

        // do nothing if no stockpile is used
        if(stockpileAmountUsed <= 0)
        {
            continue;
        }

        // decrement antivirals stockpile
        stockpile->setNum(time_+1, stockpileAmount - stockpileAmountUsed, STOCKPILE_ANTIVIRALS);

        // apply antivirals pro-rata across all stratifications

        blitz::Array<float, NUM_STRATIFICATION_DIMENSIONS> adherentTreatable(StochasticSEATIRD::numAgeGroups_, StochasticSEATIRD::numRiskGroups_, StochasticSEATIRD::numVaccinatedGroups_);
        blitz::Array<int, NUM_STRATIFICATION_DIMENSIONS> numberTreated(StochasticSEATIRD::numAgeGroups_, StochasticSEATIRD::numRiskGroups_, StochasticSEATIRD::numVaccinatedGroups_);
        blitz::Array<int, NUM_STRATIFICATION_DIMENSIONS> numberEffectivelyTreated(StochasticSEATIRD::numAgeGroups_, StochasticSEATIRD::numRiskGroups_, StochasticSEATIRD::numVaccinatedGroups_);

        // we also need the number treatable for probabilistically choosing who got the treatment
        blitz::Array<float, NUM_STRATIFICATION_DIMENSIONS> numberTreatable(StochasticSEATIRD::numAgeGroups_, StochasticSEATIRD::numRiskGroups_, StochasticSEATIRD::numVaccinatedGroups_);

        // initialize to zero, since we might not be seeing all possible stratifications
        adherentTreatable = 0.;
        numberTreated = 0;
        numberEffectivelyTreated = 0;
        numberTreatable = 0.;

        // iterate through all stratifications in priority group selections
        std::vector<std::vector<int> > stratificationValuesSet = priorityGroupSelections->getStratificationValuesSet();

        for(unsigned int s=0; s<stratificationValuesSet.size(); s++)
        {
            int a = stratificationValuesSet[s][0];
            int r = stratificationValuesSet[s][1];
            int v = stratificationValuesSet[s][2];

            std::vector<int> stratificationValues;
            stratificationValues.push_back(a);
            stratificationValues.push_back(r);
            stratificationValues.push_back(v);

            // determine number of adherent treatable
            float treatable = getValue("treatable", time_+1, nodeIds[i], stratificationValues) - getValue("treated (ineffective daily)", time_+1, nodeIds[i], stratificationValues);

            // do nothing if this population is zero
            if(treatable <= 0.)
            {
                adherentTreatable(a, r, v) = 0.;
                numberTreated(a, r, v) = 0;
                numberEffectivelyTreated(a, r, v) = 0;
                numberTreatable(a, r, v) = 0.;

                continue;
            }

            // since we fix the treatable period to one day, we can simplify our adherence calculations...
            adherentTreatable(a, r, v) = antiviralAdherence * treatable;

            // pro-rata by adherent treatable population
            numberTreated(a, r, v) = int(adherentTreatable(a, r, v) / totalAdherentTreatable * (float)stockpileAmountUsed);

            // considering effectiveness
            numberEffectivelyTreated(a, r, v) = int(antiviralEffectiveness * float(numberTreated(a, r, v)));

            // for probabilistically choosing who got the treatment
            numberTreatable(a, r, v) = treatable;

            if(numberTreated(a, r, v) <= 0)
            {
                continue;
            }

            // put_flog(LOG_DEBUG, "adherentTreatable = %f, numberTreated = %i, numberEffectivelyTreated = %i", adherentTreatable(a, r, v), numberTreated(a, r, v), numberEffectivelyTreated(a, r, v));

            // transition those effectively treated from "treatable" to "recovered"
            transition(numberEffectivelyTreated(a, r, v), "treatable", "recovered", nodeIds[i], stratificationValues);

            // need to keep track of number treated each day
            variables_["treated (daily)"](time_+1, nodeIdToIndex_[nodeIds[i]], a, r, v) += numberTreated(a, r, v);

            // need to keep track of number ineffectively treated each day
            variables_["treated (ineffective daily)"](time_+1, nodeIdToIndex_[nodeIds[i]], a, r, v) += (numberTreated(a, r, v) - numberEffectivelyTreated(a, r, v));

            // need to keep track of those treated (regardless of effectiveness)
            variables_["treated"](time_+1, nodeIdToIndex_[nodeIds[i]], a, r, v) += numberTreated(a, r, v);
        }

        // the sum over numberTreated should equal stockpileAmountUsed
        // this can differ due to integer division issues with pro rata distributions
        if(blitz::sum(numberTreated) != stockpileAmountUsed)
        {
            put_flog(LOG_WARN, "numberTreated != stockpileAmountUsed (%i != %i)", blitz::sum(numberTreated), stockpileAmountUsed);
        }

        // now, adjust schedules for individuals that were effectively treated
        // this will stop their transitions to other states and also their contact events
        boost::heap::pairing_heap<StochasticSEATIRDSchedule, boost::heap::compare<StochasticSEATIRDSchedule::compareByNextEventTime> >::iterator begin = scheduleEventQueues_[nodeIds[i]].begin();
        boost::heap::pairing_heap<StochasticSEATIRDSchedule, boost::heap::compare<StochasticSEATIRDSchedule::compareByNextEventTime> >::iterator end = scheduleEventQueues_[nodeIds[i]].end();

        boost::heap::pairing_heap<StochasticSEATIRDSchedule, boost::heap::compare<StochasticSEATIRDSchedule::compareByNextEventTime> >::iterator it;

        for(it=begin; it!=end && blitz::sum(numberEffectivelyTreated) > 0; it++)
        {
            if((*it).getState() == T)
            {
                std::vector<int> stratificationValues = (*it).getStratificationValues();

                if(numberEffectivelyTreated(BOOST_PP_ENUM(NUM_STRATIFICATION_DIMENSIONS, VECTOR_TO_ARGS, stratificationValues)) > 0)
                {
                    if((*it).canceled() != true && rand_.rand() <= float(numberEffectivelyTreated(BOOST_PP_ENUM(NUM_STRATIFICATION_DIMENSIONS, VECTOR_TO_ARGS, stratificationValues))) / numberTreatable(BOOST_PP_ENUM(NUM_STRATIFICATION_DIMENSIONS, VECTOR_TO_ARGS, stratificationValues)))
                    {
                        // cancel the remaining schedule
                        (*boost::heap::pairing_heap<StochasticSEATIRDSchedule, boost::heap::compare<StochasticSEATIRDSchedule::compareByNextEventTime> >::s_handle_from_iterator(it)).cancel();

                        numberEffectivelyTreated(BOOST_PP_ENUM(NUM_STRATIFICATION_DIMENSIONS, VECTOR_TO_ARGS, stratificationValues))--;
                    }

                    numberTreatable(BOOST_PP_ENUM(NUM_STRATIFICATION_DIMENSIONS, VECTOR_TO_ARGS, stratificationValues))--;
                }
            }
        }

        // the sum over numberEffectivelyTreated should now be zero if all events were unqueued
        if(blitz::sum(numberEffectivelyTreated) != 0)
        {
            put_flog(LOG_WARN, "numberEffectivelyTreated != 0 (%i)", blitz::sum(numberEffectivelyTreated));
        }
    }
}

void StochasticSEATIRD::applyVaccinesToPriorityGroupSelections(boost::shared_ptr<PriorityGroupSelections> priorityGroupSelections)
{
    // TODO: need to consider deceased in adherent individual totals! they reduce the adherent unvaccinated population

    if(priorityGroupSelections == NULL || priorityGroupSelections->getPriorityGroups().size() == 0)
    {
        put_flog(LOG_DEBUG, "no priority groups in selection");
        return;
    }

    double vaccineAdherence = g_parameters.getVaccineAdherence();
    double vaccineCapacity = g_parameters.getVaccineCapacity();

    // treatments for each node
    std::vector<int> nodeIds = getNodeIds();

    for(unsigned int i=0; i<nodeIds.size(); i++)
    {
        boost::shared_ptr<Stockpile> stockpile = getStockpileNetwork()->getNodeStockpile(nodeIds[i]);

        // do nothing if no stockpile is found
        if(stockpile == NULL)
        {
            continue;
        }

        // available vaccines stockpile
        int stockpileAmount = stockpile->getNum(time_+1, STOCKPILE_VACCINES);

        // do nothing if we have no available stockpile
        if(stockpileAmount == 0)
        {
            continue;
        }

        // the total populations below correspond to the priority group selections

        // determine total number of adherent unvaccinated
        float totalPopulation = getValue("population", time_+1, nodeIds[i], priorityGroupSelections->getStratificationValuesSet2(STRATIFICATIONS_ALL));
        float totalVaccinatedPopulation = getValue("population", time_+1, nodeIds[i], priorityGroupSelections->getStratificationValuesSet2(1)); // vaccinated == 1
        float totalUnvaccinatedPopulation = getValue("population", time_+1, nodeIds[i], priorityGroupSelections->getStratificationValuesSet2(0)); // unvaccinated == 0

        // do nothing if this population is zero
        if(totalUnvaccinatedPopulation <= 0.)
        {
            continue;
        }

        float totalAdherentUnvaccinated = (vaccineAdherence * totalPopulation - totalVaccinatedPopulation);

        // we will use all of our available stockpile (subject to capacity constraint) to treat the adherent unvaccinated population
        // note that we're treating all compartments, not just susceptible
        int stockpileAmountUsed = stockpileAmount;

        if(stockpileAmountUsed > (int)totalAdherentUnvaccinated)
        {
            stockpileAmountUsed = (int)totalAdherentUnvaccinated;
        }

        // capacity corresponds to total population, not just for these priority group selections
        float capacityTotalPopulation = getValue("population", time_+1, nodeIds[i]);

        // consider capacity used in previous treatments on this day
        float todayUsedCapacity = blitz::sum(variables_["vaccinated (daily)"](time_+1, nodeIdToIndex_[nodeIds[i]], blitz::Range::all(), blitz::Range::all(), 1));

        if(stockpileAmountUsed > (int)(vaccineCapacity * capacityTotalPopulation - todayUsedCapacity))
        {
            stockpileAmountUsed = (int)(vaccineCapacity * capacityTotalPopulation - todayUsedCapacity);
        }

        // do nothing if no stockpile is used
        if(stockpileAmountUsed <= 0)
        {
            continue;
        }

        // decrement vaccines stockpile
        stockpile->setNum(time_+1, stockpileAmount - stockpileAmountUsed, STOCKPILE_VACCINES);

        // apply vaccines pro-rata across all compartments and stratifications

        // these are the compartments we'll apply to
        // don't apply to deceased...
        // this MUST align with stateToCompartmentIndex below
        std::vector<std::string> compartments;
        compartments.push_back("susceptible");
        compartments.push_back("exposed");
        compartments.push_back("asymptomatic");
        compartments.push_back("treatable");
        compartments.push_back("infectious");
        compartments.push_back("recovered");

        // number vaccinated for (compartment, age group, risk group)
        blitz::Array<int, 1 + NUM_STRATIFICATION_DIMENSIONS-1> numberVaccinated(compartments.size(), StochasticSEATIRD::numAgeGroups_, StochasticSEATIRD::numRiskGroups_);

        // number vaccinatable for (compartment, age group, risk group)
        // for probabilistically choosing which event schedules to change stratifications
        blitz::Array<int, 1 + NUM_STRATIFICATION_DIMENSIONS-1> numberVaccinatable(compartments.size(), StochasticSEATIRD::numAgeGroups_, StochasticSEATIRD::numRiskGroups_);

        // initialize to zero, since we might not be seeing all possible stratifications
        numberVaccinated = 0;
        numberVaccinatable = 0;

        for(unsigned int c=0; c<compartments.size(); c++)
        {
            blitz::Array<float, NUM_STRATIFICATION_DIMENSIONS-1> adherentCompartmentUnvaccinated(StochasticSEATIRD::numAgeGroups_, StochasticSEATIRD::numRiskGroups_);

            adherentCompartmentUnvaccinated = 0.;

            // iterate through all stratifications in priority group selections (only for age group, risk group)
            std::vector<std::vector<int> > stratificationValuesSet2 = priorityGroupSelections->getStratificationValuesSet2(STRATIFICATIONS_ALL);

            for(unsigned int s=0; s<stratificationValuesSet2.size(); s++)
            {
                int a = stratificationValuesSet2[s][0];
                int r = stratificationValuesSet2[s][1];

                std::vector<int> stratificationValues(3, STRATIFICATIONS_ALL);
                stratificationValues[0] = a;
                stratificationValues[1] = r;

                // determine number of adherent compartment unvaccinated
                stratificationValues[2] = STRATIFICATIONS_ALL;
                float population = getValue("population", time_+1, nodeIds[i], stratificationValues);

                stratificationValues[2] = 1; // vaccinated
                float vaccinatedPopulation = getValue("population", time_+1, nodeIds[i], stratificationValues);

                stratificationValues[2] = 0; // unvaccinated
                float unvaccinatedPopulation = getValue("population", time_+1, nodeIds[i], stratificationValues);
                float compartmentUnvaccinated = getValue(compartments[c], time_+1, nodeIds[i], stratificationValues);

                // for probabilistically choosing which event schedules to change stratifications
                numberVaccinatable((int)c, a, r) = int(compartmentUnvaccinated);

                // do nothing if this population is zero
                if(unvaccinatedPopulation <= 0.)
                {
                    adherentCompartmentUnvaccinated(a, r) = 0.;
                    numberVaccinated((int)c, a, r) = 0;

                    continue;
                }

                // == (adherent unvaccinated population) * (fraction of unvaccinated population that is in compartment)
                adherentCompartmentUnvaccinated(a, r) = (vaccineAdherence * population - vaccinatedPopulation) * compartmentUnvaccinated / unvaccinatedPopulation;

                // pro-rata by adherent compartment unvaccinated population
                numberVaccinated((int)c, a, r) = int(adherentCompartmentUnvaccinated(a, r) / totalAdherentUnvaccinated * (float)stockpileAmountUsed);

                if(numberVaccinated((int)c, a, r) <= 0)
                {
                    continue;
                }

                // put_flog(LOG_DEBUG, "adherentCompartmentUnvaccinated = %f, numberVaccinated = %i", adherentCompartmentUnvaccinated(a, r), numberVaccinated((int)c, a, r));

                // move individuals from compartment unvaccinated to compartment vaccinated
                variables_[compartments[c]](time_+1, nodeIdToIndex_[nodeIds[i]], a, r, 0) -= numberVaccinated((int)c, a, r);
                variables_[compartments[c]](time_+1, nodeIdToIndex_[nodeIds[i]], a, r, 1) += numberVaccinated((int)c, a, r);

                // need to also manipulate the total population variable: individuals are changing stratifications as well as state
                variables_["population"](time_+1, nodeIdToIndex_[nodeIds[i]], a, r, 0) -= numberVaccinated((int)c, a, r);
                variables_["population"](time_+1, nodeIdToIndex_[nodeIds[i]], a, r, 1) += numberVaccinated((int)c, a, r);

                // need to keep track of number vaccinated each day
                variables_["vaccinated (daily)"](time_+1, nodeIdToIndex_[nodeIds[i]], a, r, 1) += numberVaccinated((int)c, a, r);
            }
        }

        // the sum over numberVaccinated should equal stockpileAmountUsed
        // this can differ due to integer division issues with pro rata distributions
        if(blitz::sum(numberVaccinated) != stockpileAmountUsed)
        {
            put_flog(LOG_WARN, "numberVaccinated != stockpileAmountUsed (%i != %i)", blitz::sum(numberVaccinated), stockpileAmountUsed);
        }

        // no need to adjust schedules since susceptible individuals are not scheduled yet, and vaccination has no effect on exposed+ individuals
        // however, we are changing individuals to the vaccinated stratification, so we need to modify schedules' fromStratificationValues!

        // we only need to do this for event types originating with one of the vaccinated compartments
        // these are "exposed", "asymptomatic", "treatable", "infectious", "recovered"
        // this MUST align with compartments above
        // in reality only E, A, T, I will be used
        std::map<StochasticSEATIRDScheduleState, int> stateToCompartmentIndex;
        stateToCompartmentIndex[E] = 1;
        stateToCompartmentIndex[A] = 2;
        stateToCompartmentIndex[T] = 3;
        stateToCompartmentIndex[I] = 4;
        stateToCompartmentIndex[R] = 5;

        boost::heap::pairing_heap<StochasticSEATIRDSchedule, boost::heap::compare<StochasticSEATIRDSchedule::compareByNextEventTime> >::iterator begin = scheduleEventQueues_[nodeIds[i]].begin();
        boost::heap::pairing_heap<StochasticSEATIRDSchedule, boost::heap::compare<StochasticSEATIRDSchedule::compareByNextEventTime> >::iterator end = scheduleEventQueues_[nodeIds[i]].end();

        boost::heap::pairing_heap<StochasticSEATIRDSchedule, boost::heap::compare<StochasticSEATIRDSchedule::compareByNextEventTime> >::iterator it;

        for(it=begin; it!=end && blitz::sum(numberVaccinated) > 0; it++)
        {
            StochasticSEATIRDScheduleState state = (*it).getState();

            if(stateToCompartmentIndex.count(state) > 0)
            {
                int c = stateToCompartmentIndex[state];

                std::vector<int> stratificationValues = (*it).getStratificationValues();

                // only consider unvaccinated for stratification change
                // vaccinated stratification == 1
                if(stratificationValues[2] == 1)
                {
                    continue;
                }

                if(numberVaccinated(c, stratificationValues[0], stratificationValues[1]) > 0)
                {
                    if((*it).canceled() != true && rand_.rand() <= float(numberVaccinated(c, stratificationValues[0], stratificationValues[1])) / float(numberVaccinatable(c, stratificationValues[0], stratificationValues[1])))
                    {
                        // change stratification to vaccinated
                        // vaccinated stratification == 1
                        stratificationValues[2] = 1;

                        (*boost::heap::pairing_heap<StochasticSEATIRDSchedule, boost::heap::compare<StochasticSEATIRDSchedule::compareByNextEventTime> >::s_handle_from_iterator(it)).changeStratificationValues(stratificationValues);

                        numberVaccinated(c, stratificationValues[0], stratificationValues[1])--;
                    }

                    numberVaccinatable(c, stratificationValues[0], stratificationValues[1])--;
                }
            }
        }

        // the sum over numberVaccinated will not necessarily be zero now, since not all vaccinated individuals had schedules
    }
}

int StochasticSEATIRD::getPopulationInVaccineLatencyPeriod(int nodeId, int ageGroup, int riskGroup)
{
    // should match the derived variable method above

    int vaccineLatencyPeriod = g_parameters.getVaccineLatencyPeriod();

    int total = 0;

    // people are vaccinated in the "morning", changing the daily count for time_+1
    // therefore we start in that bin when we're counting vaccinations
    // with these inequalities, a 0 day latency period will always return 0, as expected
    for(int t=time_+1; t>=0 && t>(time_+1 - vaccineLatencyPeriod); t--)
    {
        // vaccinated stratification == 1
        total += int(variables_["vaccinated (daily)"](t, nodeIdToIndex_[nodeId], ageGroup, riskGroup, 1));
    }

    return total;
}

void StochasticSEATIRD::travel()
{
    // TODO: review where travel() is called time-wise, and which time indices it uses here!

    // todo: these should be parameters defined elsewhere
    double RHO = 0.39;

    double sigma[] = { 1.00, 0.98, 0.94, 0.91, 0.66 };

    double contact[5][5] = {    { 45.1228487783,8.7808312353,11.7757947836,6.10114751268,4.02227175596 },
                                { 8.7808312353,41.2889143668,13.3332813497,7.847051289,4.22656343551 },
                                { 11.7757947836,13.3332813497,21.4270155984,13.7392636644,6.92483172729 },
                                { 6.10114751268,7.847051289,13.7392636644,18.0482119252,9.45371062356 },
                                { 4.02227175596,4.22656343551,6.92483172729,9.45371062356,14.0529294262 }   };

    double vaccineEffectiveness = g_parameters.getVaccineEffectiveness();

    for(unsigned int sinkNodeIndex=0; sinkNodeIndex < nodeIds_.size(); sinkNodeIndex++)
    {
        int sinkNodeId = nodeIds_[sinkNodeIndex];

        double populationSink = populationNodes_(nodeIdToIndex_[sinkNodeId]);

        std::vector<double> unvaccinatedProbabilities(StochasticSEATIRD::numAgeGroups_, 0.0);

        std::vector<double> ageBasedFlowReductions (5, 1.0);
        ageBasedFlowReductions[0] = 10; // 0-4  year olds
        ageBasedFlowReductions[1] = 2;  // 5-24 year olds
        ageBasedFlowReductions[4] = 2;  // 65+  year olds

        for(unsigned int sourceNodeIndex=0; sourceNodeIndex < nodeIds_.size(); sourceNodeIndex++)
        {
            int sourceNodeId = nodeIds_[sourceNodeIndex];

            double populationSource = populationNodes_(nodeIdToIndex_[sourceNodeId]);

            // pre-compute some frequently needed quantities
            std::vector<double> asymptomatics(StochasticSEATIRD::numAgeGroups_);
            std::vector<double> transmittings(StochasticSEATIRD::numAgeGroups_);

            for(int age=0; age<StochasticSEATIRD::numAgeGroups_; age++)
            {
                asymptomatics[age] = getValue("asymptomatic", time_+1, sourceNodeId, std::vector<int>(1,age));
                transmittings[age] = asymptomatics[age] + getValue("treatable", time_+1, sourceNodeId, std::vector<int>(1,age)) + getValue("infectious", time_+1, sourceNodeId, std::vector<int>(1,age));
            }

            if(sinkNodeId != sourceNodeId)
            {
                // flow data
                float travelFractionIJ = getTravel(sinkNodeId, sourceNodeId);
                float travelFractionJI = getTravel(sourceNodeId, sinkNodeId);

                if(travelFractionIJ > 0. || travelFractionJI > 0.)
                {
                    for(int a=0; a<StochasticSEATIRD::numAgeGroups_; a++)
                    {
                        double numberOfInfectiousContactsIJ = 0.;
                        double numberOfInfectiousContactsJI = 0.;

                        // todo: beta should be age-specific considering PHA's
                        double beta = g_parameters.getR0() / g_parameters.getBetaScale();

                        for(int b=0; b<StochasticSEATIRD::numAgeGroups_; b++)
                        {
                            double asymptomatic = asymptomatics[b];

                            double transmitting = transmittings[b];

                            double contactRate = contact[a][b];

                            double npiEffectivenessAtI = Npi::getNpiEffectiveness(g_parameters.getNpis(), sinkNodeId, int(now_), a, b);
                            double npiEffectivenessAtJ = Npi::getNpiEffectiveness(g_parameters.getNpis(), sourceNodeId, int(now_), a, b);

                            numberOfInfectiousContactsIJ += (1. - npiEffectivenessAtJ) * transmitting * beta * RHO * contactRate * sigma[a] / ageBasedFlowReductions[a];
                            numberOfInfectiousContactsJI += (1. - npiEffectivenessAtI) * asymptomatic * beta * RHO * contactRate * sigma[a] / ageBasedFlowReductions[b];
                        }

                        unvaccinatedProbabilities[a] += travelFractionIJ * numberOfInfectiousContactsIJ / populationSource;
                        unvaccinatedProbabilities[a] += travelFractionJI * numberOfInfectiousContactsJI / populationSink;
                    }
                }
            }
        }

        for(int a=0; a<StochasticSEATIRD::numAgeGroups_; a++)
        {
            for(int r=0; r<StochasticSEATIRD::numRiskGroups_; r++)
            {
                for(int v=0; v<StochasticSEATIRD::numVaccinatedGroups_; v++)
                {
                    double probability = unvaccinatedProbabilities[a];

                    // vaccinated stratification == 1
                    if(v == 1)
                    {
                        // determine vaccinated populations for this (age group, risk group):
                        // - those in the latency period
                        // - total vaccinated
                        // - => those with effective vaccinations
                        int ageRiskVaccinatedLatencyPopulationSize = getPopulationInVaccineLatencyPeriod(sinkNodeId, a, r);
                        int ageRiskVaccinatedPopulationSize = populations_(nodeIdToIndex_[sinkNodeId], a, r, 1);

                        int ageRiskVaccinatedEffectivePopulationSize = ageRiskVaccinatedPopulationSize - ageRiskVaccinatedLatencyPopulationSize;

                        // the "effective" vaccine effectiveness is weighted by the fraction of the vaccinated population with effective vaccinations
                        double effectiveVaccineEffectiveness = vaccineEffectiveness * (double)ageRiskVaccinatedEffectivePopulationSize / (double)ageRiskVaccinatedPopulationSize;

                        probability *= (1. - effectiveVaccineEffectiveness);
                    }

                    std::vector<int> stratificationValues;
                    stratificationValues.push_back(a);
                    stratificationValues.push_back(r);
                    stratificationValues.push_back(v);

                    int sinkNumSusceptible = (int)(variables_["susceptible"](time_+1, nodeIdToIndex_[sinkNodeId], a, r, v) + 0.5); // continuity correction

                    if(sinkNumSusceptible > 0)
                    {
                        int numberOfExposures = (int)gsl_ran_binomial(randGenerator_, probability, sinkNumSusceptible);

                        expose(numberOfExposures, sinkNodeId, stratificationValues);
                    }
                }
            }
        }
    }
}

void StochasticSEATIRD::precompute(int time)
{
    cachedTime_ = time;

    blitz::Array<double, 1> populationNodes(numNodes_); // [nodeIndex]

    blitz::TinyVector<int, 1+NUM_STRATIFICATION_DIMENSIONS> shape;
    shape(0) = numNodes_;
    shape(1) = StochasticSEATIRD::numAgeGroups_;
    shape(2) = StochasticSEATIRD::numRiskGroups_;
    shape(3) = StochasticSEATIRD::numVaccinatedGroups_;

    blitz::Array<double, 1+NUM_STRATIFICATION_DIMENSIONS> populations(shape); // [nodeIndex, a, r, v]

    for(unsigned int i=0; i<nodeIds_.size(); i++)
    {
        int nodeId = nodeIds_[i];

        populationNodes((int)i) = getValue("population", time, nodeId);

        for(int a=0; a<StochasticSEATIRD::numAgeGroups_; a++)
        {
            for(int r=0; r<StochasticSEATIRD::numRiskGroups_; r++)
            {
                for(int v=0; v<StochasticSEATIRD::numVaccinatedGroups_; v++)
                {
                    std::vector<int> stratificationValues;
                    stratificationValues.push_back(a);
                    stratificationValues.push_back(r);
                    stratificationValues.push_back(v);

                    populations((int)i, a, r, v) = getValue("population", time, nodeId, stratificationValues);
                }
            }
        }
    }

    populationNodes_.reference(populationNodes);
    populations_.reference(populations);
}

int StochasticSEATIRD::getScheduleCount(const int &nodeId, const StochasticSEATIRDScheduleState &state, const std::vector<int> &stratificationValues)
{
    int count = 0;

    boost::heap::pairing_heap<StochasticSEATIRDSchedule, boost::heap::compare<StochasticSEATIRDSchedule::compareByNextEventTime> >::iterator begin = scheduleEventQueues_[nodeId].begin();
    boost::heap::pairing_heap<StochasticSEATIRDSchedule, boost::heap::compare<StochasticSEATIRDSchedule::compareByNextEventTime> >::iterator end = scheduleEventQueues_[nodeId].end();

    boost::heap::pairing_heap<StochasticSEATIRDSchedule, boost::heap::compare<StochasticSEATIRDSchedule::compareByNextEventTime> >::iterator it;

    for(it=begin; it!=end; it++)
    {
        if((*it).canceled() != true && (*it).getState() == state && (*it).getStratificationValues() == stratificationValues)
        {
            count++;
        }
    }

    return count;
}

bool StochasticSEATIRD::verifyScheduleCounts()
{
    bool verified = true;

    std::vector<int> nodeIds = getNodeIds();

    for(unsigned int i=0; i<nodeIds.size(); i++)
    {
        for(int a=0; a<StochasticSEATIRD::numAgeGroups_; a++)
        {
            for(int r=0; r<StochasticSEATIRD::numRiskGroups_; r++)
            {
                for(int v=0; v<StochasticSEATIRD::numVaccinatedGroups_; v++)
                {
                    std::vector<int> stratificationValues;
                    stratificationValues.push_back(a);
                    stratificationValues.push_back(r);
                    stratificationValues.push_back(v);

                    // only verify exposed, asymptomatic, treatable, infectious, as these are the only states having events

                    int exposed = (int)getValue("exposed", time_+1, nodeIds[i], stratificationValues);
                    int exposedScheduled = getScheduleCount(nodeIds[i], E, stratificationValues);

                    int asymptomatic = (int)getValue("asymptomatic", time_+1, nodeIds[i], stratificationValues);
                    int asymptomaticScheduled = getScheduleCount(nodeIds[i], A, stratificationValues);

                    int treatable = (int)getValue("treatable", time_+1, nodeIds[i], stratificationValues);
                    int treatableScheduled = getScheduleCount(nodeIds[i], T, stratificationValues);

                    int infectious = (int)getValue("infectious", time_+1, nodeIds[i], stratificationValues);
                    int infectiousScheduled = getScheduleCount(nodeIds[i], I, stratificationValues);

                    if(exposed != exposedScheduled)
                    {
                        put_flog(LOG_ERROR, "exposed != exposedScheduled (%i != %i)", exposed, exposedScheduled);
                        verified = false;
                    }

                    if(asymptomatic != asymptomaticScheduled)
                    {
                        put_flog(LOG_ERROR, "asymptomatic != asymptomaticScheduled (%i != %i)", asymptomatic, asymptomaticScheduled);
                        verified = false;
                    }

                    if(treatable != treatableScheduled)
                    {
                        put_flog(LOG_ERROR, "treatable != treatableScheduled (%i != %i)", treatable, treatableScheduled);
                        verified = false;
                    }

                    if(infectious != infectiousScheduled)
                    {
                        put_flog(LOG_ERROR, "infectious != infectiousScheduled (%i != %i)", infectious, infectiousScheduled);
                        verified = false;
                    }
                }
            }
        }
    }

    return verified;
}
