#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/network-module.h"
#include "ns3/packet-sink.h"
#include "ns3/csma-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/multichannel-probe-module.h"

#include <unordered_map>
#include <ctime>
#include <iomanip>
#include <random>
using namespace ns3;

NS_LOG_COMPONENT_DEFINE("TCP_DYNAMIC");

class TopoHelper {

  public:
    static NodeContainer allNodes;
    static std::unordered_map<uint64_t, NetDeviceContainer> netList;
    static std::unordered_map<uint64_t, Ipv4InterfaceContainer> ifList;

    static void 
      Init(uint32_t n) {
        allNodes.Create(n);
      }

    static uint64_t 
      BuildIndex(uint32_t i, uint32_t j) {
        if (i > j) {
          uint32_t tmp = i;
          i = j;
          j = tmp;
        }
        uint64_t result = i;
        result <<= 16;
        result |= j;
        return result;
      }

    static NetDeviceContainer & 
      GetLink(uint32_t i, uint32_t j) {
        return netList[TopoHelper::BuildIndex(i, j)];
      }

    static Ipv4InterfaceContainer & 
      GetIf(uint32_t i, uint32_t j) {
        return ifList[TopoHelper::BuildIndex(i, j)];
      }

    static NodeContainer 
      GetContainerOf(std::initializer_list<uint32_t> args) {

        NodeContainer r;
        for (auto i: args) {
          r.Add(allNodes.Get(i));
        }
        return r;
      }

    static NodeContainer 
      Connect(PointToPointHelper & p2p, uint32_t i, uint32_t j) {

        NodeContainer n = TopoHelper::GetContainerOf({i, j});
        netList[TopoHelper::BuildIndex(i, j)] = p2p.Install(n);

        return n; 
      }

    static void 
      Connect(PointToPointHelper & p2p, 
          NodeContainer & nodes,
          uint32_t i, 
          uint32_t j) { 

        netList[TopoHelper::BuildIndex(i, j)] = p2p.Install(nodes);

      }

    static void
      AssignIP(Ipv4AddressHelper & iphelper, uint32_t i, uint32_t j) {

        ifList[TopoHelper::BuildIndex(i, j)] = 
          iphelper.Assign(netList[TopoHelper::BuildIndex(i, j)]);

      }

    static void 
      ChangeBW(uint64_t rate, uint32_t i, uint32_t j) {

        NS_LOG_INFO("Rate changing to " << rate);

        auto d = TopoHelper::GetLink(i, j);
        auto dev = d.Get(0);
        DynamicCast<PointToPointNetDevice>(dev)->SetDataRate(DataRate(rate));
        dev = d.Get(1);
        DynamicCast<PointToPointNetDevice>(dev)->SetDataRate(DataRate(rate));

      }

    static void
      LinkUp(uint32_t i, uint32_t j) {

        auto if0 = ifList[TopoHelper::BuildIndex(i, j)].Get(0);
        if0.first->GetObject<Ipv4L3Protocol>()->GetInterface(if0.second)->SetUp();
        auto if1 = ifList[TopoHelper::BuildIndex(i, j)].Get(1);
        if1.first->GetObject<Ipv4L3Protocol>()->GetInterface(if1.second)->SetUp();

      }

    static void
      LinkDown(uint32_t i, uint32_t j) {

        auto if0 = ifList[TopoHelper::BuildIndex(i, j)].Get(0);
        if0.first->GetObject<Ipv4L3Protocol>()->GetInterface(if0.second)->SetDown();
        auto if1 = ifList[TopoHelper::BuildIndex(i, j)].Get(1);
        if1.first->GetObject<Ipv4L3Protocol>()->GetInterface(if1.second)->SetDown();

      }

};

NodeContainer TopoHelper::allNodes = NodeContainer();
std::unordered_map<uint64_t, NetDeviceContainer> TopoHelper::netList =
std::unordered_map<uint64_t, NetDeviceContainer>();
std::unordered_map<uint64_t, Ipv4InterfaceContainer> TopoHelper::ifList = 
std::unordered_map<uint64_t, Ipv4InterfaceContainer>();

std::ofstream bwlog;
std::ofstream qllog;
std::ofstream drlog;
std::ofstream rxlog;
std::ofstream frlog;
std::ofstream cwndlog;
std::ofstream lolog;
std::ofstream synlog;
std::ofstream estlog;
std::ofstream config;

std::string bwt_str;
std::string delay = "10us";
std::string q_size = "110p";
std::string l_inter = "100us";
std::vector<std::pair<uint64_t, uint64_t>> bwt;

double nsd = 5000.0;
uint64_t max_bytes = 20000000000;
uint64_t simtime = 60;
uint64_t h_rate = 100000000000;
uint64_t rwnd = 262144;
uint64_t nflows = 1;
bool nochange = false;
bool indivlog = true;


static int curr_rate = 0;
void CycleRate() {

  /*
     if (bwlog.is_open()) 
     bwlog << Simulator::Now().GetNanoSeconds() << ", " << bwt[curr_rate].first << std::endl;
     */

  curr_rate = (curr_rate + 1) % bwt.size();
  TopoHelper::ChangeBW(bwt[curr_rate].first, 2, 3);

  /*
     if (bwlog.is_open()) 
     bwlog << Simulator::Now().GetNanoSeconds() << ", " << bwt[curr_rate].first << std::endl;
     */

  NS_LOG_INFO("Next rate change scheduled, will change to " << bwt[(curr_rate + 1) % bwt.size()].first);

  Simulator::Schedule(MicroSeconds(bwt[curr_rate].second), CycleRate);
}

void QlTrace (std::string ctxt, uint32_t oldValue, uint32_t newValue)
{
  //std::cout << "At time " << Simulator::Now().GetSeconds() << "s " << ctxt << ": QueueLength: " << oldValue << " to " << newValue << std::endl;

  if (qllog.is_open()) {
    if (ctxt[10] == '2')
      qllog << Simulator::Now().GetNanoSeconds() << ", 2, " << newValue << std::endl;
    else if (ctxt[10] == '3')
      qllog << Simulator::Now().GetNanoSeconds() << ", 3, " << newValue << std::endl;
  }
}

std::vector<uint64_t> dropped;
void DropTrace (std::string ctxt, const Ptr<QueueDiscItem const> qip)
{
  // qip->Print(std::cout);
  if (drlog.is_open()) {
    if (ctxt[10] == '2')
      qllog << Simulator::Now().GetNanoSeconds() << ", 2, " << ++dropped[0] << std::endl;
    else if (ctxt[10] == '3')
      qllog << Simulator::Now().GetNanoSeconds() << ", 3, " << ++dropped[1] << std::endl;
  }
}

uint64_t recved = 0; 
uint64_t final_fct = 0;
void RxTrace(std::string ctxt, Ptr<const Packet> p, const Address &address) 
{
  uint64_t now = Simulator::Now().GetNanoSeconds();
  if (now > final_fct)
    final_fct = now;
}

uint64_t cwnd_reduction = 0;
static void CwndTrace (std::string ctxt, uint32_t oldValue, uint32_t newValue)
{
  if (cwndlog.is_open()) 
    cwndlog <<  Simulator::Now().GetNanoSeconds() << ", " << ctxt << ", " << newValue << std::endl;
  // std::cout << "At time " << Simulator::Now().GetSeconds() << "s Flow "<< ctxt << ": cwnd " << oldValue << " -> " << newValue << std::endl;
  // Simulator::Now().GetNanoSeconds() << ", " << newValue << std::endl;
  if (newValue < oldValue)
    cwnd_reduction++;
}

uint64_t cong_fastr = 0;
uint64_t cong_loss  = 0;
std::vector<uint64_t> frdata;
std::vector<uint64_t> lodata;

static void
CongStateTrace (std::string ctxt, 
    const TcpSocketState::TcpCongState_t oldValue, 
    const TcpSocketState::TcpCongState_t newValue)
{

  if (oldValue != TcpSocketState::CA_RECOVERY && newValue == TcpSocketState::CA_RECOVERY) {
    cong_fastr++;
    frlog <<  Simulator::Now().GetNanoSeconds() << ", " << ctxt << ", " << ++frdata[stoi(ctxt)] << std::endl;
  }

  if (oldValue != TcpSocketState::CA_LOSS && newValue == TcpSocketState::CA_LOSS) {
    cong_loss++;
    lolog <<  Simulator::Now().GetNanoSeconds() << ", " << ctxt << ", " << ++lodata[stoi(ctxt)] << std::endl;
  }

}

std::vector<uint64_t> estdata;

static void
TcpStateTrace (std::string ctxt, 
    const TcpSocket::TcpStates_t oldValue, 
    const TcpSocket::TcpStates_t newValue)
{

  if (oldValue != TcpSocket::ESTABLISHED && newValue == TcpSocket::ESTABLISHED) {
    estlog <<  Simulator::Now().GetNanoSeconds() << ", " << ctxt << ", " << ++estdata[stoi(ctxt)] << std::endl;
  }

}

std::vector<uint64_t> syndata;
static void TxPktTrace (std::string ctxt,  
    const Ptr< const Packet > packet, 
    const TcpHeader &header, 
    const Ptr< const TcpSocketBase > socket)
{
  if (header.GetFlags() & TcpHeader::SYN) {

    if (synlog.is_open()) 
      synlog <<  Simulator::Now().GetNanoSeconds() << ", " << ctxt << ", " << ++syndata[stoi(ctxt)] << std::endl;
    // std::cout << "At time " << Simulator::Now().GetSeconds() << "s Flow "<< ctxt << ": cwnd " << oldValue << " -> " << newValue << std::endl;
    // Simulator::Now().GetNanoSeconds() << ", " << newValue << std::endl;
  }
}

void ParseBWP(std::string & p) 
{
  std::istringstream ss(p);
  int c = 0;
  uint64_t r = 0, t = 0;

  while (ss) {

    std::string next;
    if (!getline(ss, next, ',')) break;

    if (c & 1) {
      t = std::stoull(next);
      bwt.push_back(std::make_pair(r, t));
    }
    else {
      r = std::stoull(next);
    }

    c++; // :)

  }

}

int main(int argc, char * argv[]) {

  // Config::SetDefault ("ns3::Ipv4GlobalRouting::RespondToInterfaceEvents", BooleanValue (true));
  LogComponentEnable("TcpCongestionOps", LOG_LEVEL_INFO);

  CommandLine cmd;
  cmd.AddValue("BWP", "Bandwidth pattern", bwt_str);
  cmd.AddValue("QueueLength", "Queue length of router", q_size);
  cmd.AddValue("HostRate", "Link rate between host and TOR", h_rate);
  cmd.AddValue("MaxBytes", "Number of tests to test", max_bytes);
  cmd.AddValue("PropDelay", "Propagation delay", delay);
  cmd.AddValue("LogInterval", "interval between mcp probe", l_inter);
  cmd.AddValue("IndivLog", "Enable individual log", indivlog);
  cmd.AddValue("Static", "If the topology should be static. Will use the first linkrate in specified pattern", nochange);
  cmd.AddValue("SimTime", "Max simulation time", simtime);
  cmd.AddValue("RWND", "Receiver windown size", rwnd);
  cmd.AddValue("NFlows", "Number of flows", nflows);
  cmd.AddValue("Nsd", "standard deviation for flow start time", nsd);
  cmd.Parse(argc, argv);

  ParseBWP(bwt_str);

  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);
  std::ostringstream oss;

  if (indivlog) {
    /*
       oss << "./bwlog" << std::put_time(&tm, "_%m_%d_%Y_%H_%M_%S");
       bwlog.open(oss.str());
       */

    oss.str("");
    oss << "./qllog" << std::put_time(&tm, "_%m_%d_%Y_%H_%M_%S");
    qllog.open(oss.str());

    oss.str("");
    oss << "./drlog" << std::put_time(&tm, "_%m_%d_%Y_%H_%M_%S");
    drlog.open(oss.str());

    /*
    oss.str("");
    oss << "./rxlog" << std::put_time(&tm, "_%m_%d_%Y_%H_%M_%S");
    rxlog.open(oss.str());
    */

    oss.str("");
    oss << "./frlog" << std::put_time(&tm, "_%m_%d_%Y_%H_%M_%S");
    frlog.open(oss.str());

    oss.str("");
    oss << "./lolog" << std::put_time(&tm, "_%m_%d_%Y_%H_%M_%S");
    lolog.open(oss.str());

    oss.str("");
    oss << "./cslog" << std::put_time(&tm, "_%m_%d_%Y_%H_%M_%S");
    cwndlog.open(oss.str());

    oss.str("");
    oss << "./sylog" << std::put_time(&tm, "_%m_%d_%Y_%H_%M_%S");
    synlog.open(oss.str());

    oss.str("");
    oss << "./eslog" << std::put_time(&tm, "_%m_%d_%Y_%H_%M_%S");
    estlog.open(oss.str());
  }

  oss.str("");
  oss << "./config" << std::put_time(&tm, "_%m_%d_%Y_%H_%M_%S");
  config.open(oss.str());

  config << std::put_time(&tm, "%m_%d_%Y_%H_%M_%S") << std::endl;
  config << "HostRate " << h_rate << std::endl;
  config << "PropDelay " << delay << std::endl;
  config << "MaxBytes " << max_bytes << std::endl;
  config << "QueueLen " << q_size << std::endl;
  config << "Rwnd " << rwnd << std::endl;
  config << "NFlows " << nflows << std::endl;
  config << "BWP " << bwt_str << std::endl;
  config << "NSD " << nsd << " ";

  nflows *= 2;

  Time::SetResolution(Time::NS);
  LogComponentEnable("TCP_DYNAMIC", LOG_LEVEL_INFO);
  LogComponentEnable("PacketSink", LOG_LEVEL_INFO);
  LogComponentEnable("BulkSendApplication", LOG_LEVEL_INFO);

  TopoHelper::Init(4);

  PointToPointHelper hp2p;
  PointToPointHelper sp2p;
  hp2p.SetDeviceAttribute("DataRate", DataRateValue(DataRate(h_rate)));
  hp2p.SetDeviceAttribute("Mtu", UintegerValue(1522));
  hp2p.SetChannelAttribute("Delay", StringValue(delay));
  // hp2p.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue ("1p"));
  TopoHelper::Connect(hp2p, 0, 2);
  TopoHelper::Connect(hp2p, 1, 3);

  // PointToPointHelper sp2p;
  sp2p.SetDeviceAttribute("DataRate", DataRateValue(DataRate(bwt[0].first)));
  sp2p.SetDeviceAttribute("Mtu", UintegerValue(1522));
  sp2p.SetChannelAttribute("Delay", StringValue(delay));
  sp2p.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue ("1p"));

  TopoHelper::Connect(sp2p, 2, 3);

  InternetStackHelper stack;
  stack.Install(TopoHelper::allNodes);

  TrafficControlHelper tch;
  uint16_t handle = tch.SetRootQueueDisc("ns3::FifoQueueDisc");
  tch.AddInternalQueues(handle, 1, "ns3::DropTailQueue", "MaxSize", StringValue (q_size.c_str()));
  tch.Install(TopoHelper::GetLink(2, 3).Get(0));
  tch.Install(TopoHelper::GetLink(2, 3).Get(1));

  Ipv4AddressHelper address;
  address.SetBase("10.0.0.0", "255.255.255.0");
  TopoHelper::AssignIP(address, 0, 2);
  address.SetBase("10.0.1.0", "255.255.255.0");
  TopoHelper::AssignIP(address, 1, 3);
  address.SetBase("10.1.0.0", "255.255.255.0");
  TopoHelper::AssignIP(address, 2, 3);

  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  Ptr<FlowMonitor> flowMonitor;
  FlowMonitorHelper flowHelper;
  flowMonitor = flowHelper.InstallAll();

  /*
     oss.str("");
     oss << "all" << std::put_time(&tm, "_%m_%d_%Y_%H_%M_%S") << ".csv";

     Ptr<MultichannelProbe> mcp = CreateObject<MultichannelProbe> (oss.str());
     mcp->SetAttribute ("Interval", StringValue(l_inter.c_str()));
     mcp->AttachAll ();
     */

  frdata = std::vector<uint64_t>(nflows, 0);
  lodata = std::vector<uint64_t>(nflows, 0);
  syndata = std::vector<uint64_t>(nflows, 0);
  estdata = std::vector<uint64_t>(nflows, 0);
  dropped = std::vector<uint64_t>(2, 0);

  Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue (1448));

  Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue (rwnd));
  Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue (rwnd));

  std::random_device rd;
  std::default_random_engine       gen(rd());
  std::normal_distribution<double> nd(0.0, nsd);
  double startdiff;

  uint16_t portInit = 2048;  

  /*
     ApplicationContainer sourceApps[nflows];
     BulkSendHelper source ("ns3::TcpSocketFactory",
     InetSocketAddress (TopoHelper::GetIf(1, 3).GetAddress(0), portInit));
     source.SetAttribute ("MaxBytes", UintegerValue(max_bytes/nflows));
     */

  Address sinkAddressr(InetSocketAddress(TopoHelper::GetIf(1, 3).GetAddress(0), portInit));
  std::vector< Ptr<BulkSendApplication> > sendAppsr;

  Address sinkAddressl(InetSocketAddress(TopoHelper::GetIf(0, 2).GetAddress(0), portInit));
  std::vector< Ptr<BulkSendApplication> > sendAppsl;

  uint64_t i = 0;
  for (; i < nflows; i++) {

    Ptr<Socket> ns3TcpSocket = Socket::CreateSocket (TopoHelper::allNodes.Get(0), TcpSocketFactory::GetTypeId());
    ns3TcpSocket->TraceConnect ("CongestionWindow", std::to_string(i),  MakeCallback (&CwndTrace));
    ns3TcpSocket->TraceConnect ("CongState", std::to_string(i),  MakeCallback (&CongStateTrace));
    ns3TcpSocket->TraceConnect ("State", std::to_string(i),  MakeCallback (&TcpStateTrace));
    ns3TcpSocket->TraceConnect ("Tx", std::to_string(i),  MakeCallback (&TxPktTrace));

    Ptr<BulkSendApplication> app = CreateObject<BulkSendApplication> ();
    app->SetUp(ns3TcpSocket, sinkAddressr, max_bytes/nflows);
    TopoHelper::allNodes.Get(0)->AddApplication(app);

    startdiff = nd(gen);
    if ((startdiff = nd(gen)) < -999999)
      startdiff = -999999;

    app->SetStartTime (MicroSeconds (1000000.0 + startdiff));

    sendAppsl.push_back(app);

    /*
       sourceApps[i] = source.Install(TopoHelper::allNodes.Get(0));
       sourceApps[i].Start(MicroSeconds(1000000.0 + startdiff));
       */

    config << startdiff << ", ";

  }
  i++;

  for (; i < nflows; i++) {

    Ptr<Socket> ns3TcpSocket = Socket::CreateSocket (TopoHelper::allNodes.Get(1), TcpSocketFactory::GetTypeId());
    ns3TcpSocket->TraceConnect ("CongestionWindow", std::to_string(i),  MakeCallback (&CwndTrace));
    ns3TcpSocket->TraceConnect ("CongState", std::to_string(i),  MakeCallback (&CongStateTrace));
    ns3TcpSocket->TraceConnect ("State", std::to_string(i),  MakeCallback (&TcpStateTrace));
    ns3TcpSocket->TraceConnect ("Tx", std::to_string(i),  MakeCallback (&TxPktTrace));

    Ptr<BulkSendApplication> app = CreateObject<BulkSendApplication> ();
    app->SetUp(ns3TcpSocket, sinkAddressl, max_bytes/nflows);
    TopoHelper::allNodes.Get(1)->AddApplication(app);

    startdiff = nd(gen);
    if ((startdiff = nd(gen)) < -999999)
      startdiff = -999999;

    app->SetStartTime (MicroSeconds (1000000.0 + startdiff));

    sendAppsr.push_back(app);

    /*
       sourceApps[i] = source.Install(TopoHelper::allNodes.Get(0));
       sourceApps[i].Start(MicroSeconds(1000000.0 + startdiff));
       */

    config << startdiff << ", ";

  }
  config << std::endl;

  PacketSinkHelper sink("ns3::TcpSocketFactory",
      InetSocketAddress (Ipv4Address::GetAny(), portInit));
  ApplicationContainer sinkAppr = sink.Install(TopoHelper::allNodes.Get(1));
  sinkAppr.Start (Seconds (0.0));
  ApplicationContainer sinkAppl = sink.Install(TopoHelper::allNodes.Get(0));
  sinkAppl.Start (Seconds (0.0));
  /*
     ApplicationContainer sourceApp1 = source.Install(TopoHelper::allNodes.Get(0));
     sourceApp1.Start (Seconds(1.0));
     ApplicationContainer sourceApp2 = source.Install(TopoHelper::allNodes.Get(0));
     sourceApp2.Start (MicroSeconds(1000000.0 + flowStartDt));
  // sourceApps.Stop (Seconds(20.0));
  */

  // Create a PacketSinkApplication and install it on node 3
  // sinkApps.Stop (Seconds(20.0));

  Config::Connect(
      "/NodeList/2/$ns3::TrafficControlLayer/RootQueueDiscList/1/BytesInQueue",
      MakeCallback (&QlTrace));
  Config::Connect(
      "/NodeList/3/$ns3::TrafficControlLayer/RootQueueDiscList/1/BytesInQueue",
      MakeCallback (&QlTrace));
  Config::Connect(
      "/NodeList/2/$ns3::TrafficControlLayer/RootQueueDiscList/1/Drop",
      MakeCallback (&DropTrace));
  Config::Connect(
      "/NodeList/3/$ns3::TrafficControlLayer/RootQueueDiscList/1/Drop",
      MakeCallback (&DropTrace));
  Config::Connect ("/NodeList/*/ApplicationList/*/$ns3::PacketSink/Rx",
      MakeCallback (&RxTrace));


  // Set up tracing if enabled
  // if (tracing)
  // {
  /*
     AsciiTraceHelper ascii;
     oss.str("");
     oss << "asciitr" << std::put_time(&tm, "_%m_%d_%Y_%H_%M_%S");
     hp2p.EnableAsciiAll (ascii.CreateFileStream (oss.str()));

     oss.str("");
     oss << "pcap" << std::put_time(&tm, "_%m_%d_%Y_%H_%M_%S");
     hp2p.EnablePcap(oss.str(), TopoHelper::allNodes.Get(0)->GetId(), 0);
  // }
  */
  if (!nochange)
    Simulator::Schedule(MicroSeconds(1000000+bwt[0].second), CycleRate);

  Simulator::Stop(Seconds(simtime));
  Simulator::Run();

  oss.str("");
  oss << "mon" << std::put_time(&tm, "_%m_%d_%Y_%H_%M_%S") << ".xml";
  flowMonitor->SerializeToXmlFile(oss.str(), true, true);

  Simulator::Destroy();

  if (indivlog) {
    //bwlog.close();
    qllog.close();
    drlog.close();
    rxlog.close();
    frlog.close();
    lolog.close();
    synlog.close();
    estlog.close();
    cwndlog.close();
  }
  //cwndlog.close();
  config << "cwnd_reduce " << cwnd_reduction << std::endl;
  config << "to_CA_RECOVERY " << cong_fastr << std::endl;
  config << "to_CA_LOSS " << cong_loss << std::endl;
  config << "FCT " << final_fct << std::endl;
  config.close();

  return 0;

}
