#!/usr/bin/python

from mininet.net import Mininet
from mininet.node import Controller
from mininet.link import TCLink
from mininet.cli import CLI
from mininet.log import setLogLevel

from subprocess import call
import os, time, threading

""" Application related function """

def sendingIperfTraffic(nodes, name):
    print "*** Starting iPerf Server on host ***"
    server = nodes['h2']
    server.cmd('iperf -s &')
    if not os.path.exists(name):
        os.mkdir(name)
        user = os.getenv('SUDO_USER')
        os.system('sudo chown -R '+user+':'+user+' '+name)
    server.cmd('tcpdump -i h2-eth0 -w '+name+'/server.pcap &')

    print "*** Starting iPerf Clients on stations ***"
    time.sleep(1)
    client = nodes['h1']
    client.cmdPrint('iperf -c 10.0.0.2 -t 30')

""" Main function of the simulation """

def mobileNet(name, conges, delay):

    print("*** System configuration ***\n")
    # Configuring the congestion control
    if conges == 'bbr':
        os.system('sudo sysctl -w net.core.default_qdisc=fq')
    else:
        os.system('sudo sysctl -w net.core.default_qdisc=pfifo_fast')
    os.system('sudo sysctl -w net.ipv4.tcp_congestion_control='+conges)

    net = Mininet(controller=Controller, link=TCLink, autoSetMacs=True)

    print("*** Creating nodes ***")
    nodes = {}

    node = net.addHost('h1')
    nodes['h1'] = node
    node = net.addHost('h2')
    nodes['h2'] = node
    node = net.addSwitch('s1')
    nodes['s1'] = node
    node = net.addSwitch('s2')
    nodes['s2'] = node
    node = net.addSwitch('s3')
    nodes['s3'] = node
    node = net.addSwitch('s4')
    nodes['s4'] = node

    net.addLink(nodes['h1'], nodes['s1'])
    net.addLink(nodes['s1'], nodes['s2'])
    net.addLink(nodes['s2'], nodes['s3'], bw=10, delay=str(delay)+'ms', loss=0.0001)
    net.addLink(nodes['s3'], nodes['s4'])
    net.addLink(nodes['s4'], nodes['h2'])

    node = net.addController('c0')
    nodes['c0'] = node

    print("*** Starting network simulation ***")
    net.start()

    # CLI(net)

    print "*** Starting to generate the traffic ***"
    traffic_thread = threading.Thread(target=sendingIperfTraffic, args=(nodes, name))
    traffic_thread.start()
    traffic_thread.join()

    print("*** Stopping network ***")
    net.stop()


if __name__ == '__main__':
    print("*** *** *** *** *** *** *** *** *** *** ***")
    print("***                                     ***")
    print("***  Welcome to the Mininet simulation  ***")
    print("***                                     ***")
    print("*** *** *** *** *** *** *** *** *** *** ***\n")
    while True:
        print("--- Available congestion control: ")
        print("reno\tcubic\tbbr")
        conges = raw_input('--- Please select: ')
        if conges == 'reno' or conges == 'cubic' or conges == 'bbr':
            break

    while True:
        delay = raw_input('--- Please input the delay (ms): ')
        break

    while True:
        name = raw_input('--- Please name this testing: ')
        break

    setLogLevel('info')

    mobileNet('results/'+name, conges, delay)

