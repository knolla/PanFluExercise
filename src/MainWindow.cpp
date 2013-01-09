#include "MainWindow.h"
#include "MapWidget.h"
#include "EpidemicSimulation.h"
#include "EpidemicDataSet.h"
#include "ParametersWidget.h"
#include "EpidemicInitialCasesWidget.h"
#include "EpidemicInfoWidget.h"
#include "EpidemicChartWidget.h"
#include "models/disease/StochasticSEATIRD.h"

MainWindow::MainWindow()
{
    // defaults
    time_ = 0;

    // create menus in menu bar
    QMenu * fileMenu = menuBar()->addMenu("&File");

    // create tool bars
    QToolBar * toolbar = addToolBar("toolbar");

    QToolBar * toolbarBottom = new QToolBar("bottom toolbar", this);
    addToolBar(Qt::BottomToolBarArea, toolbarBottom);

    // new simulation action
    QAction * newSimulationAction = new QAction("New Simulation", this);
    newSimulationAction->setStatusTip("New simulation");
    connect(newSimulationAction, SIGNAL(triggered()), this, SLOT(newSimulation()));

    // open data set action
    QAction * openDataSetAction = new QAction("Open Data Set", this);
    openDataSetAction->setStatusTip("Open data set");
    connect(openDataSetAction, SIGNAL(triggered()), this, SLOT(openDataSet()));

    // new chart action
    QAction * newChartAction = new QAction("New Chart", this);
    newChartAction->setStatusTip("New chart");
    connect(newChartAction, SIGNAL(triggered()), this, SLOT(newChart()));

    // add actions to menus
    fileMenu->addAction(newSimulationAction);
    fileMenu->addAction(openDataSetAction);
    fileMenu->addAction(newChartAction);

    // add actions to toolbar
    toolbar->addAction(newSimulationAction);
    toolbar->addAction(openDataSetAction);
    toolbar->addAction(newChartAction);

    // make a map widget the main view
    mapWidget_ = new MapWidget();
    setCentralWidget(mapWidget_);

    // setup time slider and add it to bottom toolbar with label
    timeSlider_ = new QSlider(Qt::Horizontal, this);
    connect(timeSlider_, SIGNAL(valueChanged(int)), this, SLOT(setTime(int)));
    toolbarBottom->addWidget(new QLabel("Time"));
    toolbarBottom->addWidget(timeSlider_);

    // previous timestep button
    QAction * previousTimestepAction = new QAction(QIcon::fromTheme("media-seek-backward"), "Previous Timestep", this);
    previousTimestepAction->setStatusTip(tr("Previous timestep"));
    connect(previousTimestepAction, SIGNAL(triggered()), this, SLOT(previousTimestep()));
    toolbarBottom->addAction(previousTimestepAction);

    // play timesteps button
    playTimestepsAction_ = new QAction(QIcon::fromTheme("media-seek-play"), "Play Timesteps", this);
    playTimestepsAction_->setStatusTip(tr("Play timesteps"));
    playTimestepsAction_->setCheckable(true);
    connect(playTimestepsAction_, SIGNAL(toggled(bool)), this, SLOT(playTimesteps(bool)));
    toolbarBottom->addAction(playTimestepsAction_);

    // next timestep button
    QAction * nextTimestepAction = new QAction(QIcon::fromTheme("media-seek-forward"), "Next Timestep", this);
    nextTimestepAction->setStatusTip(tr("Next timestep"));
    connect(nextTimestepAction, SIGNAL(triggered()), this, SLOT(nextTimestep()));
    toolbarBottom->addAction(nextTimestepAction);

    // parameters dock
    QDockWidget * parametersDockWidget = new QDockWidget("Parameters", this);
    parametersDockWidget->setWidget(new ParametersWidget());
    addDockWidget(Qt::LeftDockWidgetArea, parametersDockWidget);

    // initial cases dock
    initialCasesWidget_ = new EpidemicInitialCasesWidget(this);
    QDockWidget * initialCasesDockWidget = new QDockWidget("Initial Cases", this);
    initialCasesDockWidget->setWidget(initialCasesWidget_);
    addDockWidget(Qt::LeftDockWidgetArea, initialCasesDockWidget);

    // info dock
    QDockWidget * infoDockWidget = new QDockWidget("Info", this);
    infoDockWidget->setWidget(new EpidemicInfoWidget(this));
    addDockWidget(Qt::LeftDockWidgetArea, infoDockWidget);

    // tabify parameters, initial cases, and info docks
    tabifyDockWidget(parametersDockWidget, initialCasesDockWidget);
    tabifyDockWidget(parametersDockWidget, infoDockWidget);

    // chart docks
    QDockWidget * chartDockWidget = new QDockWidget("Chart", this);
    chartDockWidget->setWidget(new EpidemicChartWidget(this));
    addDockWidget(Qt::BottomDockWidgetArea, chartDockWidget);

    chartDockWidget = new QDockWidget("Chart", this);
    chartDockWidget->setWidget(new EpidemicChartWidget(this));
    addDockWidget(Qt::BottomDockWidgetArea, chartDockWidget);

    // make other signal / slot connections
    connect(this, SIGNAL(dataSetChanged(boost::shared_ptr<EpidemicDataSet>)), mapWidget_, SLOT(setDataSet(boost::shared_ptr<EpidemicDataSet>)));

    connect(this, SIGNAL(dataSetChanged()), this, SLOT(resetTimeSlider()));

    connect(this, SIGNAL(timeChanged(int)), mapWidget_, SLOT(setTime(int)));

    connect(&playTimestepsTimer_, SIGNAL(timeout()), this, SLOT(playTimesteps()));

    // show the window
    show();
}

MainWindow::~MainWindow()
{
    delete mapWidget_;
}

QSize MainWindow::sizeHint() const
{
    return QSize(1024, 768);
}

void MainWindow::setTime(int time)
{
    time_ = time;

    // make sure the time slider has the correct value
    // Qt makes sure this won't result in infinite recursion
    timeSlider_->setValue(time_);

    emit(timeChanged(time_));
}

bool MainWindow::previousTimestep()
{
    if(dataSet_ != NULL)
    {
        int previousTime = time_ - 1;

        bool timeInBounds = true;

        if(previousTime < 0)
        {
            timeInBounds = false;
        }

        if(timeInBounds == true)
        {
            setTime(previousTime);

            return true;
        }
    }

    return false;
}

void MainWindow::playTimesteps(bool set)
{
    if(set == true)
    {
        if(playTimestepsTimer_.isActive() == true)
        {
            bool success = nextTimestep();

            if(success != true)
            {
                // uncheck the play button
                playTimestepsAction_->setChecked(false);
            }
        }
        else
        {
            // start the timer
            playTimestepsTimer_.start(PLAY_TIMESTEPS_TIMER_DELAY_MILLISECONDS);
        }
    }
    else
    {
        // stop the timer
        playTimestepsTimer_.stop();
    }

    // wait for any GUI events to be processed
    QCoreApplication::processEvents();
}

bool MainWindow::nextTimestep()
{
    if(dataSet_ != NULL)
    {
        int nextTime = time_ + 1;

        bool timeInBounds = true;

        boost::shared_ptr<EpidemicSimulation> simulation = boost::dynamic_pointer_cast<EpidemicSimulation>(dataSet_);

        if(simulation != NULL)
        {
            // the data set is actually a simulation

            if(nextTime >= simulation->getNumTimes())
            {
                // if this is the first time simulated, set the initial cases
                if(nextTime == 1)
                {
                    initialCasesWidget_->applyCases();
                }

                simulation->simulate();

                // since we've changed the number of timesteps
                emit(dataSetChanged(dataSet_));
            }
        }
        else
        {
            // just a regular data set
            if(nextTime >= dataSet_->getNumTimes())
            {
                timeInBounds = false;
            }
        }

        if(timeInBounds == true)
        {
            setTime(nextTime);

            return true;
        }
    }

    return false;
}

void MainWindow::newSimulation()
{
    // use StochasticSEATIRD model
    boost::shared_ptr<EpidemicSimulation> simulation(new StochasticSEATIRD());

    dataSet_ = simulation;

    emit(dataSetChanged(dataSet_));
}

void MainWindow::openDataSet()
{
    QString filename = QFileDialog::getOpenFileName(this, "Open Data Set", "", "Simulation files (*.nc)");

    if(!filename.isEmpty())
    {
        boost::shared_ptr<EpidemicDataSet> dataSet(new EpidemicDataSet(filename.toStdString().c_str()));

        if(dataSet->isValid() != true)
        {
            QMessageBox::warning(this, "Error", "Could not load data set.", QMessageBox::Ok, QMessageBox::Ok);
        }
        else
        {
            dataSet_ = dataSet;

            emit(dataSetChanged(dataSet_));
        }
    }
}

void MainWindow::newChart()
{
    QDockWidget * chartDockWidget = new QDockWidget("Chart", this);

    EpidemicChartWidget * epidemicChartWidget = new EpidemicChartWidget(this);

    chartDockWidget->setWidget(epidemicChartWidget);

    addDockWidget(Qt::BottomDockWidgetArea, chartDockWidget);

    chartDockWidget->setFloating(true);

    if(dataSet_ != NULL)
    {
        epidemicChartWidget->setDataSet(dataSet_);
        epidemicChartWidget->setTime(time_);
    }
}

void MainWindow::resetTimeSlider()
{
    if(dataSet_ != NULL)
    {
        timeSlider_->setMinimum(0);
        timeSlider_->setMaximum(dataSet_->getNumTimes() - 1);

        setTime(0);
    }
    else
    {
        timeSlider_->setMinimum(0);
        timeSlider_->setMaximum(0);
    }
}
