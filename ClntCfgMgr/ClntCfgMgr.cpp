/*                                                                           
 * Dibbler - a portable DHCPv6                                               
 *                                                                           
 * authors: Tomasz Mrugalski <thomson@klub.com.pl>                           
 *          Marek Senderski <msend@o2.pl>                                    
 *                                                                           
 * released under GNU GPL v2 or later licence                                
 *                                                                           
 * $Id: ClntCfgMgr.cpp,v 1.14 2004-05-23 21:35:31 thomson Exp $
 *
 * $Log: not supported by cvs2svn $
 * Revision 1.13  2004/05/23 21:02:43  thomson
 * *** empty log message ***
 *
 * Revision 1.12  2004/05/23 20:50:38  thomson
 * *** empty log message ***
 *
 */

#include <iostream>
#include <fstream>
#include <string>
#include "SmartPtr.h"
#include "Portable.h"
#include "ClntCfgMgr.h"
#include "ClntCfgIface.h"
#include "Logger.h"

using namespace std;

#include "IfaceMgr.h"
#include "ClntIfaceMgr.h"

#include "ClntParsGlobalOpt.h"
#include "TimeZone.h"

#include "FlexLexer.h"
#include "ClntParser.h"

TClntCfgMgr::TClntCfgMgr(SmartPtr<TClntIfaceMgr> ClntIfaceMgr, 
                         const string cfgFile,const string oldCfgFile)
    :TCfgMgr((Ptr*)ClntIfaceMgr)
{
    int result;
    ifstream f;
    this->IfaceMgr = ClntIfaceMgr;
    this->IsDone=false;

    /* support for config changes between runs - currently disabled */
    // bool newConf=false; //newConf=true if files differs
    // newConf=compareConfigs(cfgFile,oldCfgFile);
    // if(newConf) 
    //   this->copyFile(cfgFile,oldCfgFile);
    /* support for config changes between runs - currently disabled */

    // parse config file
    f.open(cfgFile.c_str());
    if ( ! f.is_open()  ) {
	Log(logCrit) << "Unable to open " << cfgFile << " file." << logger::endl; 
	this->IsDone = true;
        return;
    }
    yyFlexLexer lexer(&f,&clog);
    clntParser parser(&lexer);
    std::clog << logger::logDebug << "Parsing " << cfgFile << "..." << logger::endl;
    result = parser.yyparse();
    std::clog << logger::logDebug << "Parsing " << cfgFile << " done." << logger::endl;
    f.close();

    if (result) {
        //Result!=0 means config errors. Finish whole DHCPClient 
        Log(logCrit) << "Config error." << logger::endl;
        IsDone = true; 
        this->DUID=new TDUID();
        return;
    }

    // match parsed interfaces with interfaces detected in system
    matchParsedSystemInterfaces(&parser);

    this->WorkDir = parser.ParserOptStack.getLast()->getWorkDir();
  
    // check config consistency
    if(!checkConfigConsistency())
    {
        this->IsDone=true;
        return;
    }

    // load or create DUID
    if ( !loadDUID(this->WorkDir+"/"+(string)CLNTDUID_FILE) ) {
	this->IsDone=true;
	return;
    }
    
    // store ClntCfgMgr in file
    std::ofstream xmlDump;
    xmlDump.open(CLNTCFGMGR_FILE);
    xmlDump << *this;
    xmlDump.close();
    
    IsDone = false;
}

/*
  match parsed interfaces with interfaces detected in system. 
  CfgIface objects are created placed in CfgMgr. 
*/
bool TClntCfgMgr::matchParsedSystemInterfaces(clntParser *parser) {
    int cfgIfaceCnt;
    cfgIfaceCnt = parser->ClntCfgIfaceLst.count();
    Log(logDebug) << cfgIfaceCnt << " interface(s) specified in " << CLNTCFGMGR_FILE << logger::endl;

    SmartPtr<TClntCfgIface> cfgIface;
    SmartPtr<TIfaceIface> ifaceIface;

    if (cfgIfaceCnt) {
	// user specified some interfaces in config file
	parser->ClntCfgIfaceLst.first();
	while(cfgIface = parser->ClntCfgIfaceLst.get()) {
	    // for each interface (from config file)
	    if (cfgIface->getID()==-1) {
		ifaceIface = IfaceMgr->getIfaceByName(cfgIface->getName());
	    } else {
		ifaceIface = IfaceMgr->getIfaceByID(cfgIface->getID());
	    }

	    if (!ifaceIface) {
		Log(logError) << "Interface " << cfgIface->getName() << "/" << cfgIface->getID() 
			      << " specified in " << CLNTCFGMGR_FILE << " is not present in the system."
			      << logger::endl;
		continue;
	    }
	    if (cfgIface->noConfig()) {
		Log(logInfo) << "Interface " << cfgIface->getName() << "/" << cfgIface->getID() 
			       << " has flag no-config set, so it is ignored." << logger::endl;
		continue;
	    }

	    cfgIface->setIfaceName(ifaceIface->getName());
	    cfgIface->setIfaceID(ifaceIface->getID());

	    this->addIface(cfgIface);
	    Log(logInfo) << "Interface " << cfgIface->getName() << "/" << cfgIface->getID() 
			 << " has been added." << logger::endl;
	}
    } else {
	// user didn't specified any interfaces in config file, so
	// we'll try to configure each interface we could find
	Log(logWarning) << CLNTCFGMGR_FILE << " does not contain any interface definitions."
			<< logger::endl;
	
	IfaceMgr->firstIface();
	while ( ifaceIface = IfaceMgr->getIface() ) {
	    // for each interface present in the system...
	    if (!ifaceIface->flagUp()) {
		Log(logNotice) << "Interface " << ifaceIface->getName() << "/" << ifaceIface->getID() 
			       << " is down, ignoring." << logger::endl;
		continue;
	    }
	    if (!ifaceIface->flagRunning()) {
		Log(logNotice) << "Interface " << ifaceIface->getName() << "/" << ifaceIface->getID() 
			       << " has flag RUNNING not set, ignoring." << logger::endl;
		continue;
	    }
	    if (!ifaceIface->flagMulticast()) {
		Log(logNotice) << "Interface " << ifaceIface->getName() << "/" << ifaceIface->getID() 
			       << " is not multicast capable, ignoring." << logger::endl;
		continue;
	    }
	    if ( !(ifaceIface->getMacLen() > 5) ) {
		Log(logNotice) << "Interface " << ifaceIface->getName() << "/" << ifaceIface->getID() 
			       << " has MAC address length " << ifaceIface->getMacLen() 
			       << ", but greater than 5 is required, ignoring." << logger::endl;
		continue;
	    }
	    
	    cfgIface = SmartPtr<TClntCfgIface>(new TClntCfgIface(ifaceIface->getID()));
	    cfgIface->setIfaceName(ifaceIface->getName());
	    cfgIface->setIfaceID(ifaceIface->getID());
	    
	    SmartPtr<TClntCfgGroup> group(new TClntCfgGroup());
	    group->setOptions(parser->ParserOptStack.getLast());
	    SmartPtr<TClntCfgIA> ia(new TClntCfgIA(
					parser->ParserOptStack.getLast()->getIncedIAIDCnt()));
	    ia->setOptions(parser->ParserOptStack.getLast());
	    SmartPtr<TClntCfgAddr> addr(new TClntCfgAddr());
	    addr->setOptions(parser->ParserOptStack.getLast());
	    //append created iface, group and address
	    cfgIface->addGroup(group);
	    group->addIA(ia);
	    ia->addAddr(addr);
	    this->addIface(cfgIface);
	    Log(logInfo) << "Interface " << cfgIface->getName() << "/" << cfgIface->getID() 
			 << " has been added." << logger::endl;
	}
    }
    return true;
}

SmartPtr<TClntCfgIface> TClntCfgMgr::getIface()
{
    return ClntCfgIfaceLst.get();
}

void TClntCfgMgr::addIface(SmartPtr<TClntCfgIface> ptr)
{
    ClntCfgIfaceLst.append(ptr);
}

void TClntCfgMgr::firstIface()
{
    ClntCfgIfaceLst.first();
}

int TClntCfgMgr::countIfaces()
{
    return ClntCfgIfaceLst.count();
}

string TClntCfgMgr::getWorkDir()
{
    return WorkDir;
}

bool TClntCfgMgr::getReconfigure()
{
    //FIXME
    return false;
}

string TClntCfgMgr::getLogName()
{
    return LogName;
}

int TClntCfgMgr::getLogLevel()
{
    return LogLevel;
}

int TClntCfgMgr::countAddrForIA(long IAID)
{
    SmartPtr<TClntCfgIface> ptrIface;
    firstIface();
    while (ptrIface = getIface() ) 
    {
        SmartPtr<TClntCfgGroup> ptrGroup;
        ptrIface->firstGroup();
        while (ptrGroup = ptrIface->getGroup() ) 
        {
            SmartPtr<TClntCfgIA> ptrIA;
            ptrGroup->firstIA();
            while (ptrIA = ptrGroup->getIA())
                if (ptrIA->getIAID()==IAID)
                    return ptrIA->countAddr();
        }
    }    
    return 0;
}

SmartPtr<TClntCfgIA> TClntCfgMgr::getIA(long IAID)
{
    SmartPtr<TClntCfgIface> ptrIface;
    firstIface();
    while (ptrIface = getIface() ) 
    {
        SmartPtr<TClntCfgGroup> ptrGroup;
        ptrIface->firstGroup();
        while (ptrGroup = ptrIface->getGroup() ) 
        {
            SmartPtr<TClntCfgIA> ptrIA;
            ptrGroup->firstIA();
            while (ptrIA = ptrGroup->getIA())
                if (ptrIA->getIAID()==IAID)
                    return ptrIA;
        }
    }        
    return SmartPtr<TClntCfgIA>();
}

bool TClntCfgMgr::setIAState(int iface, int iaid, enum EState state)
{
    firstIface();
    SmartPtr<TClntCfgIface> ptrIface;
    while (ptrIface = getIface() ) {
        if ( ptrIface->getID() == iface ) break;
    }
    if (! ptrIface) {
        std::clog << logger::logError <<"ClntCfgMgr: Unable to set IA state (id=" << iaid 
            << "):Interface " << iface << " not found." << logger::endl;
        return false;
    }

    SmartPtr<TClntCfgGroup> ptrGroup;
    ptrIface->firstGroup();

    while (ptrGroup = ptrIface->getGroup() ) {
        SmartPtr<TClntCfgIA> ptrIA;
        ptrGroup->firstIA();
        while (ptrIA = ptrGroup->getIA()) 
        {
            if ( ptrIA->getIAID() == iaid ) {
                ptrIA->setState(state);
                return true;
            }
        }
    }
    std::clog << logger::logError << "ClntCfgMgr: Unable to set IA state (id=" 
        << iaid << ")" << logger::endl;
    return false;
}	    

//check whether T1<T2 and Pref<Valid and at least T1<=Valid
bool TClntCfgMgr::checkConfigConsistency()
{
    //Is everything so far is ok
    if (IsDone) return false;
    SmartPtr<TClntCfgIface> ptrIface;
    this->ClntCfgIfaceLst.first();
    while(ptrIface=ClntCfgIfaceLst.get())
    {
        SmartPtr<TClntCfgGroup> ptrGroup;
        ptrIface->firstGroup();
        if(ptrIface->isReqTimeZone()&&(ptrIface->getProposedTimeZone()!=""))
        {   
            TTimeZone tmp(ptrIface->getProposedTimeZone());
            if(!tmp.isValid())
            {
                this->IsDone=true;
                clog<<logger::logCrit
                    <<"Wrong time zone option for:"
                    <<"iface(id/name)"<<ptrIface->getID()<<"/"
                    <<ptrIface->getName() << logger::endl;
                return !(IsDone=true);
            }
        }

        while(ptrGroup=ptrIface->getGroup())
        {
            SmartPtr<TClntCfgIA> ptrIA;
            ptrGroup->firstIA();
            while(ptrIA=ptrGroup->getIA())
            {
                if ((unsigned long)ptrIA->getT2()<(unsigned long)ptrIA->getT1()) 
                {
                    clog<<logger::logCrit
                        <<"T1 can't be lower than T2 for IA:"<<*ptrIA << logger::endl
                        <<"in iface(id/name)"<<ptrIface->getID()<<"/"
                        <<ptrIface->getName() << logger::endl;
                    return !(IsDone=true);
                }
                SmartPtr<TClntCfgAddr> ptrAddr;
                ptrIA->firstAddr();
                while(ptrAddr=ptrIA->getAddr())
                {
                    if((unsigned long)ptrAddr->getPref()>(unsigned long)ptrAddr->getValid())
                    {
                        clog<<logger::logCrit
                        <<"Prefered time:" <<ptrAddr->getPref()
                        <<"can't be lower than Valid lifetime:"<<ptrAddr->getValid()
                        <<"for IA:"<<*ptrIA << logger::endl
                        <<"in iface(id/name)"<<ptrIface->getID()<<"/"
                        <<ptrIface->getName() << logger::endl;
                        return !(IsDone=true);
                    }
                    if ((unsigned long)ptrIA->getT1()>(unsigned long)ptrAddr->getValid())
                    {
                        clog<<logger::logCrit
                        <<"Valid lifetime:"<<ptrAddr->getValid()
                        <<"can't be lower than T1 "<<ptrIA->getT1()
                        <<"(address can't be renewed) in IA:"<<*ptrIA << logger::endl
                        <<"in iface(id/name)"<<ptrIface->getID()<<"/"
                        <<ptrIface->getName() << logger::endl;
                        return !(IsDone=true);
                    }
                }
            }
            
        }
    }
    return true;
}

SmartPtr<TClntCfgGroup> TClntCfgMgr::getGroupForIA(long IAID)
{
    SmartPtr<TClntCfgIface> iface;
    firstIface();
    while(iface=getIface())
    {
        SmartPtr<TClntCfgGroup> group;
        iface->firstGroup();
        while(group=iface->getGroup())
        {
            SmartPtr<TClntCfgIA> ia;
            group->firstIA();
            while(ia=group->getIA())
                if (ia->getIAID()==IAID)
                    return group;
        }
    }
    return SmartPtr<TClntCfgGroup>();
}

SmartPtr<TClntCfgIface> TClntCfgMgr::getIface(int id)
{
    firstIface();
    SmartPtr<TClntCfgIface> iface;
    while(iface=getIface())
        if (iface->getID()==id) return iface;
    return SmartPtr<TClntCfgIface> ();
}

SmartPtr<TClntCfgIface> TClntCfgMgr::getIfaceByIAID(int iaid)
{
    SmartPtr<TClntCfgIface> iface;
    firstIface();
    while(iface=getIface())
    {
        SmartPtr<TClntCfgGroup> group;
        iface->firstGroup();
        while(group=iface->getGroup())
        {
            SmartPtr<TClntCfgIA> ia;
            group->firstIA();
            while(ia=group->getIA())
                if (ia->getIAID()==iaid)
                    return iface;
        }
    }
    return SmartPtr<TClntCfgIface>();
}

bool TClntCfgMgr::isDone()
{
    return this->IsDone;
}

ostream & operator<<(ostream &strum, TClntCfgMgr &x)
{
    strum << "<ClntCfgMgr>" << endl;
    strum << "  " << *x.DUID;
    strum << "  <WorkDir>" << x.getWorkDir()  << "</WorkDir>" << endl;
    strum << "  <LogName>" << x.getLogName()  << "</LogName>" << endl;
    strum << "  <LogLevel>" << x.getLogLevel() << "</LogLevel>" << endl;

    SmartPtr<TClntCfgIface> ptr;
    x.firstIface();

    while ( ptr = x.getIface() ) {
        strum << *ptr;
    }

    strum << "</ClntCfgMgr>" << endl;
    return strum;
}


