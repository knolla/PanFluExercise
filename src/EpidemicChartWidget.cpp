#include "EpidemicChartWidget.h"
#include "EpidemicDataSet.h"

EpidemicChartWidget::EpidemicChartWidget(MainWindow * mainWindow)
{
    // defaults
    time_ = 0;
    nodeId_ = NODES_ALL;
    stratifyByIndex_ = -1;
    stratificationValues_ = std::vector<int>(NUM_STRATIFICATION_DIMENSIONS, STRATIFICATIONS_ALL);

    // add toolbar
    QToolBar * toolbar = addToolBar("toolbar");

    // add node choices to toolbar
    toolbar->addWidget(new QLabel("County"));
    toolbar->addWidget(&nodeComboBox_);

    connect(&nodeComboBox_, SIGNAL(currentIndexChanged(int)), this, SLOT(setNodeChoice(int)));

    toolbar->addWidget(new QLabel("Variable"));
    toolbar->addWidget(&variableComboBox_);

    connect(&variableComboBox_, SIGNAL(currentIndexChanged(int)), this, SLOT(setVariableChoice(int)));

    // toolbar line break
    addToolBarBreak();
    toolbar = addToolBar("toolbar");

    // add stratify by choices to toolbar
    std::vector<std::string> stratificationNames = EpidemicDataSet::getStratificationNames();

    stratifyByComboBox_.addItem("None", -1);

    for(unsigned int i=0; i<stratificationNames.size(); i++)
    {
        stratifyByComboBox_.addItem(QString(stratificationNames[i].c_str()), i);
    }

    connect(&stratifyByComboBox_, SIGNAL(currentIndexChanged(int)), this, SLOT(setStratifyByChoice(int)));

    toolbar->addWidget(new QLabel("Stratify by"));
    toolbar->addWidget(&stratifyByComboBox_);

    // toolbar line break
    addToolBarBreak();
    toolbar = addToolBar("toolbar");

    // add stratification choices to toolbar
    toolbar->addWidget(new QLabel("Filter by"));

    std::vector<std::vector<std::string> > stratifications = EpidemicDataSet::getStratifications();

    for(unsigned int i=0; i<stratifications.size(); i++)
    {
        QComboBox * stratificationValueComboBox = new QComboBox(this);

        stratificationValueComboBox->addItem("All", STRATIFICATIONS_ALL);

        for(unsigned int j=0; j<stratifications[i].size(); j++)
        {
            stratificationValueComboBox->addItem(QString(stratifications[i][j].c_str()), j);
        }

        stratificationValueComboBoxes_.push_back(stratificationValueComboBox);

        connect(stratificationValueComboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(changedStratificationValueChoice()));

        toolbar->addWidget(stratificationValueComboBox);
    }

    setCentralWidget(&chartWidget_);

    // make connections
    connect((QObject *)mainWindow, SIGNAL(dataSetChanged(boost::shared_ptr<EpidemicDataSet>)), this, SLOT(setDataSet(boost::shared_ptr<EpidemicDataSet>)));

    connect((QObject *)mainWindow, SIGNAL(timeChanged(int)), this, SLOT(setTime(int)));
}

void EpidemicChartWidget::setDataSet(boost::shared_ptr<EpidemicDataSet> dataSet)
{
    dataSet_ = dataSet;

    // refresh node and variable selections
    nodeComboBox_.clear();
    variableComboBox_.clear();

    if(dataSet != NULL)
    {
        // add node entries
        nodeComboBox_.addItem("All", NODES_ALL);

        std::vector<int> nodeIds = dataSet->getNodeIds();

        for(unsigned int i=0; i<nodeIds.size(); i++)
        {
            nodeComboBox_.addItem(dataSet->getNodeName(nodeIds[i]).c_str(), nodeIds[i]);
        }

        // add variable entries
        std::vector<std::string> variables = dataSet->getVariableNames();

        for(unsigned int i=0; i<variables.size(); i++)
        {
            variableComboBox_.addItem(variables[i].c_str(), variables[i].c_str());
        }
    }

    update();
}

void EpidemicChartWidget::setTime(int time)
{
    time_ = time;

    if(dataSet_ != NULL && timeIndicator_ != NULL)
    {
        // don't do a full update, just update the time indicator line
        timeIndicator_->clear();

        timeIndicator_->addPoint(time_, 0);
        timeIndicator_->addPoint(time_, 999999999.);
    }
}

void EpidemicChartWidget::setNodeId(int nodeId)
{
    nodeId_ = nodeId;

    update();
}

void EpidemicChartWidget::setVariable(std::string variable)
{
    variable_ = variable;

    update();
}

void EpidemicChartWidget::setStratifyByIndex(int index)
{
    stratifyByIndex_ = index;

    update();
}

void EpidemicChartWidget::setStratificationValues(std::vector<int> stratificationValues)
{
    stratificationValues_ = stratificationValues;

    update();
}

void EpidemicChartWidget::update()
{
    // clear current plots
    chartWidget_.clear();

    // set x-axis label
    std::string xAxisLabel("Time (days)");
    chartWidget_.setXAxisLabel(xAxisLabel);

    // set y-axis label
    std::string yAxisLabel("Population");
    chartWidget_.setYAxisLabel(yAxisLabel);

    if(dataSet_ != NULL)
    {
        // set title
        if(nodeId_ == NODES_ALL)
        {
            chartWidget_.setTitle("All Counties");
        }
        else
        {
            chartWidget_.setTitle(dataSet_->getNodeName(nodeId_) + std::string(" County"));
        }

        if(stratifyByIndex_ == -1)
        {
            // no stratifications

            // plot the variable
            boost::shared_ptr<ChartWidgetLine> line = chartWidget_.getLine();

            line->setColor(1.,0.,0.);
            line->setWidth(2.);
            line->setLabel(variable_.c_str());

            for(int t=0; t<dataSet_->getNumTimes(); t++)
            {
                line->addPoint(t, dataSet_->getValue(variable_, t, nodeId_, stratificationValues_));
            }
        }
        else if(stratifyByIndex_ != -1)
        {
            // add with stratifications

            std::vector<std::vector<std::string> > stratifications = EpidemicDataSet::getStratifications();

            // plot the variable
            boost::shared_ptr<ChartWidgetLine> line = chartWidget_.getLine(NEW_LINE, STACKED);

            line->setWidth(2.);

            std::vector<std::string> labels;

            for(unsigned int i=0; i<stratifications[stratifyByIndex_].size(); i++)
            {
                labels.push_back(variable_ + " (" + stratifications[stratifyByIndex_][i] + ")");
            }

            line->setLabels(labels);

            for(int t=0; t<dataSet_->getNumTimes(); t++)
            {
                std::vector<double> variableValues;

                for(unsigned int i=0; i<stratifications[stratifyByIndex_].size(); i++)
                {
                    std::vector<int> stratificationValues = stratificationValues_;

                    stratificationValues[stratifyByIndex_] = i;

                    variableValues.push_back(dataSet_->getValue(variable_, t, nodeId_, stratificationValues));
                }

                line->addPoints(t, variableValues);
            }
        }

        // clear time indicator
        timeIndicator_ = chartWidget_.getLine();
        timeIndicator_->setWidth(2.);
        timeIndicator_->setLabel("");

        // reset chart bounds
        chartWidget_.resetBounds();
    }
}

void EpidemicChartWidget::setNodeChoice(int choiceIndex)
{
    int index = nodeComboBox_.itemData(choiceIndex).toInt();

    setNodeId(index);
}

void EpidemicChartWidget::setVariableChoice(int choiceIndex)
{
    std::string variable = variableComboBox_.itemData(choiceIndex).toString().toStdString();

    setVariable(variable);
}

void EpidemicChartWidget::setStratifyByChoice(int choiceIndex)
{
    int index = stratifyByComboBox_.itemData(choiceIndex).toInt();

    setStratifyByIndex(index);
}

void EpidemicChartWidget::changedStratificationValueChoice()
{
    std::vector<int> stratificationValues;

    for(unsigned int i=0; i<stratificationValueComboBoxes_.size(); i++)
    {
        stratificationValues.push_back(stratificationValueComboBoxes_[i]->itemData(stratificationValueComboBoxes_[i]->currentIndex()).toInt());
    }

    setStratificationValues(stratificationValues);
}
