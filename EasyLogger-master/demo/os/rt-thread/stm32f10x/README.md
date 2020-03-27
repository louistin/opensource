# stm32f10x RT-Thread demo

---

## 1�����

ͨ�� `app\src\app_task.c` �� `test_elog()` ������������־�������Ĭ�Ͽ������첽���ģʽ���û����Խ����ն���������־�����������������á�

## 2��ʹ�÷���

�򿪵��Ե��ն���Demo�Ĵ���1�������ӣ��������� 115200 8 1 N����ʱ���ն��оͿ������� "2.6 Demo" Gif�������ᵽ�ĳ���������£�

### 2.1 ���Ĺ���

- 1��elog��ʹ����ʧ�������־��`elog on`��ʹ�ܣ�`elog off`��ʧ�ܣ���ʾ������־������࣬����������������Ե�ʱ�򣬿��Խ���־�����ʧ�ܣ���
- 2��elog_lvl�����ù��˼���(0-5)��
- 3��elog_tag�����ù��˱�ǩ������ `elog_tag+��Ҫ���˵ı�ǩ` ����ֻ�е���־�ı�ǩ�������˱�ǩʱ���Żᱻ����������κβ�������չ��˱�ǩ��
- 4��elog_kw�����ù��˹ؼ��ʣ����� `elog_kw+��Ҫ���˵Ĺؼ���` ����ֻ�е���־�� **��������** �������˹ؼ���ʱ���Żᱻ�����ֱ������ `elog_kw` ����ʱ�����κβ�����������������õĹ��˹ؼ��ʡ�

### 2.2 Flash Log������־���浽Flash�У�

��������`components\easylogger\plugins\flash\elog_flash_cfg.h`���ÿ�������ģʽ����ʱֻ�л��������˲Ż���Flash��д�롣

- 1��elog_flash read����ȡ�洢��Flash�е�������־��
 - 1.1��elog_flash read xxxx����ȡ��������xxxx�ֽڴ�С����־��
- 2��elog_flash flush�����̽��������е�������־������Flash�У�ע�⣺ֻ�п������˻��幦�ܲŻ���Ч����
- 3��elog_flash clean�����Flash�е������ѱ�����־����ʱ����������־Ҳ������ա�

## 3���ļ����У�˵��

- `components\easylogger\port\elog_port.c` ���Ĺ�����ֲ�ο��ļ�
- `components\easylogger\plugins\flash\elog_flash_port.c` Flash Log������ֲ�ο��ļ�
- `RVMDK` ��ΪKeil�����ļ�
- `EWARM` ��ΪIAR�����ļ�

## 4����������

- 1������ RTT���Լ�Ӳ���쳣�Ĺ��ӷ�����ʹ��ϵͳ��MCU�ڳ����쳣ʱ��������־��Ȼ���Ա����ͬʱ������Flash���ο� `app\src\app_task.c` �е� `rtt_user_assert_hook` �� `exception_hook` ������
- 2������ EasyLogger���ԵĹ��ӷ�����ʹ��ϵͳ�ڳ����쳣ʱ��������־��Ȼ���Ա����ͬʱ������Flash���ο� `app\src\app_task.c` �е� `elog_user_assert_hook` ������