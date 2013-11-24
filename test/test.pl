#!/usr/bin/perl

#use strict;
use DBI;
use IPC::System::Simple qw(capture);

sub trim
{
    local($strBuffer) = @_;

	$strBuffer =~ s/^\s+|\s+$//g;

	return $strBuffer;
}

sub execute
{
    local($strCommand) = @_;
    my $strOutput;

    print("$strCommand");
    $strOutput = trim(capture($strCommand));

    if ($strOutput eq "")
    {  
        print(" ... complete\n\n");
    }
    else
    {
        print(" ... complete\n$strOutput\n\n");
    }
    
    return $strOutput;
}

sub pg_create
{
    local($strPgBinPath, $strTestPath, $strTestDir, $strArchiveDir) = @_;
    
    execute("mkdir $strTestPath");
    execute($strPgBinPath . "/initdb -D $strTestPath/$strTestDir -A trust -k");
    execute("mkdir $strTestPath/$strArchiveDir");
}

sub pg_start
{
    local($strPgBinPath, $strDbPath, $strPort, $strAchiveCommand) = @_;
    my $strCommand = "$strPgBinPath/pg_ctl start -o \"-c port=$strPort -c wal_level=archive -c archive_mode=on -c archive_command=\'$strAchiveCommand\'\" -D $strDbPath -l $strDbPath/postgresql.log -w -s";
    
    execute($strCommand);
}

sub pg_password_set
{
    local($strPgBinPath, $strPath, $strUser) = @_;
    my $strCommand = "$strPgBinPath/psql --port=6000 -c \"alter user $strUser with password 'password'\" postgres";
    
    execute($strCommand);
}

sub pg_stop
{
    local($strPgBinPath, $strPath) = @_;
    my $strCommand = "$strPgBinPath/pg_ctl stop -D $strPath -w -s";
    
    execute($strCommand);
}

sub pg_drop
{
    local($strTestPath) = @_;
    my $strCommand = "rm -rf $strTestPath";
    
    execute($strCommand);
}

sub pg_execute
{
    local($dbh, $strSql) = @_;

    $sth = $dbh->prepare($strSql);
    $sth->execute();
}

my $strUser = execute('whoami');

my $strTestPath = "/Users/dsteele/test";
my $strDbDir = "db";
my $strArchiveDir = "archive";

my $strPgBinPath = "/Library/PostgreSQL/9.3/bin";

my $strBackRestBinPath = "/Users/dsteele/pg_backrest";
my $strArchiveCommand = "$strBackRestBinPath/pg_backrest.pl archive-local %p $strTestPath/$strArchiveDir/%f";

################################################################################
# Stop the current test cluster if it is running and create a new one
################################################################################
eval {pg_stop($strPgBinPath, "$strTestPath/$strDbDir")};

if ($@)
{
    print(" ... unable to stop pg server (ignoring): " . trim($@) . "\n\n")
}

pg_drop($strTestPath);
pg_create($strPgBinPath, $strTestPath, $strDbDir, $strArchiveDir);
pg_start($strPgBinPath, "$strTestPath/$strDbDir", "6000", $strArchiveCommand);
pg_password_set($strPgBinPath, "$strTestPath/$strDbDir", $strUser);

################################################################################
# Connect and start
################################################################################
$dbh = DBI->connect("dbi:Pg:dbname=postgres;port=6000;host=127.0.0.1", 'dsteele', 'password', {AutoCommit => 1});
pg_execute($dbh, "create table test (id int)");

pg_execute($dbh, "insert into test values (1)");
pg_execute($dbh, "select pg_switch_xlog()");

pg_execute($dbh, "insert into test values (2)");
pg_execute($dbh, "select pg_switch_xlog()");

pg_execute($dbh, "insert into test values (3)");
pg_execute($dbh, "select pg_switch_xlog()");

#pg_stop($strTestPath);
