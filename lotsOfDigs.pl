#!/usr/bin/perl

#
# Settings
#
$dig_addr = "127.0.0.1";
$dig_port = 53;

#
# Script
#
$num_domains = 0;
$domains[$num_domains++] = "RANDOM!";
$domains[$num_domains++] = "google.com";
$domains[$num_domains++] = "facebook.com";
$domains[$num_domains++] = "twitter.com";
$domains[$num_domains++] = "apple.com";
$domains[$num_domains++] = "ebay.com";
$domains[$num_domains++] = "paypal.com";
$domains[$num_domains++] = "amazon.com";
$domains[$num_domains++] = "youtube.com";
$domains[$num_domains++] = "yahoo.com";
$domains[$num_domains++] = "yelp.com";


$command_base = "dig \@$dig_addr -p $dig_port";
while (1)
{
	$idx = int(rand($num_domains));
	my $domain_str = "";
	
	if ($idx == 0)
	{
		# Use a random domain
		@chars = ("A".."Z", "a".."z");
		$domain_str .= $chars[rand @chars] for 1..8;
		$domain_str .= ".com";
	}
	else
	{
		# Use working domain out of our list
		$domain_str = $domains[$idx];
	}
	
	$command = "$command_base $domain_str";
	print("Running: $command\n");
	`$command`;
}
