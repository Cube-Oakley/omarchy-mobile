# SPDX-License-Identifier: MIT
import unittest
from sms_codec import pack7, unpack7, submit, deliver, received_pdu, number

class CodecTests(unittest.TestCase):
    def test_known_gsm_vector_and_extensions(self):
        self.assertEqual(pack7([104,101,108,108,111]),(5,bytes.fromhex('e8329bfd06')))
        self.assertEqual(unpack7(bytes.fromhex('e8329bfd06'),5),'hello')
        self.assertEqual(unpack7(pack7([27,101,27,20])[1],4),'€^')

    def test_long_text_headers_and_unicode_character_boundaries(self):
        for text in ('a'*161,'^'*81,'🙂'*40):
            parts=submit('+12025550123',text)
            self.assertEqual(len(parts),2)
            contents=[]
            for i,pdu in enumerate(parts):
                self.assertEqual(pdu[0],0x41)
                address_size=(pdu[2]+1)//2
                dcs=pdu[5+address_size]; length=pdu[6+address_size]; data=pdu[7+address_size:]
                self.assertEqual(data[:3],bytes([5,0,3]))
                self.assertEqual(data[4:6],bytes([2,i+1]))
                self.assertLessEqual(len(data),140)
                contents.append(unpack7(data,length,7) if dcs==0 else data[6:].decode('utf-16-be'))
            self.assertEqual(''.join(contents),text)

    def test_delivery_known_text_and_timestamp(self):
        # SMS-DELIVER, international sender, default alphabet, 2026-09-29 UTC.
        pdu=bytes.fromhex('000b912120550521f300006290922143650005e8329bfd06')
        sms=deliver(pdu)
        self.assertEqual(received_pdu(bytes.fromhex('07912120550521f3')+pdu),sms)
        self.assertEqual(sms['number'],'+12025550123')
        self.assertEqual(sms['text'],'hello')
        self.assertTrue(sms['timestamp'].startswith('2026-09-29'))
        with self.assertRaises(ValueError): deliver(pdu[:-1])

    def test_number_and_special_service_rejection(self):
        self.assertEqual(number('(202) 555-0123',voice=True),'+12025550123')
        for value in ('911','112','*123#'):
            with self.assertRaises(ValueError): number(value,voice=True)
