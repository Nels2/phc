<?php

	// Check that x is propagated into the return function
	// { phc-option: -O1 --dump=outssa }
	// { phc-regex-output: !/\$[x]/ }


	function x ($x)
	{
		$x = 7; // Not dead

		return $x;
	}

?>
